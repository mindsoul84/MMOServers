#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <atomic>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "PacketDispatcher.h"
#include "Zone/Zone.h"
#include "Monster/Monster.h"
#include "Pathfinder/Pathfinder.h"

#include "../Common/DataManager/DataManager.h"
#include "../Common/Define/GameConstants.h"

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

class GatewaySession;
class WorldConnection;

// ==========================================
// [수정] PlayerInfo에서 개별 뮤텍스(mtx) 제거
//
// 변경 전: std::mutex mtx로 개별 유저 보호
//   -> 멀티스레드에서 동시 접근 시 필요했음
//
// 변경 후: game_strand_가 모든 게임 로직을 직렬화하므로
//   PlayerInfo에 대한 동시 접근이 원천적으로 발생하지 않음
//   -> 개별 뮤텍스 불필요, 제거하여 구조체 단순화
// ==========================================
struct PlayerInfo {
    uint64_t uid;
    float x, y;
    int hp  = GameConstants::Player::DEFAULT_HP;
    int atk = GameConstants::Player::DEFAULT_ATK;
    int def = GameConstants::Player::DEFAULT_DEF;

    PlayerInfo() = default;
    PlayerInfo(const PlayerInfo&) = delete;
    PlayerInfo& operator=(const PlayerInfo&) = delete;
};

// ==========================================
// GameContext 싱글톤 + 의존성 주입(DI) 지원
//
// [스레드 모델 재설계]
//
// 변경 전: io_context.run()을 여러 워커 스레드에서 호출하면서
//   playerMutex_(shared_mutex)와 monsterMutex_(shared_mutex)로
//   게임 상태를 보호. 락 순서 규칙을 수동으로 관리해야 했으며,
//   Monster AI Tick과 플레이어 이동 간 락 경합 발생.
//
// 변경 후: game_strand_ 도입
//   - 모든 패킷 핸들러가 game_strand_에서 실행됨
//   - AI Tick 타이머 콜백이 game_strand_에서 실행됨
//   - Monster::CalculatePath 결과 콜백이 game_strand_에서 실행됨
//   -> playerMap, monsterMap, Zone 접근이 단일 스레드에서만 발생
//   -> shared_mutex, PlayerInfo::mtx 전부 제거 (락 경합 원천 제거)
//   -> 락 순서 규칙 자체가 불필요해짐 (데드락 가능성 0)
//
//   네트워크 I/O(Read/Write)는 여전히 멀티스레드로 동작하며,
//   각 세션의 Send()는 자체 strand로 직렬화됩니다.
//
// [게이트웨이 장애 복구]
//   gatewayPlayerMap_: 각 GatewaySession이 중계하는 유저 목록을 추적
//   GatewaySession 연결 해제 시 해당 유저 전원을 일괄 정리
// ==========================================
struct GameContext {
    DataManager dataManager;

    boost::asio::io_context io_context;

    // [추가] 게임 로직 전용 strand
    // 패킷 핸들러, AI Tick, 경로 계산 결과 콜백이 모두 이 strand에서 실행됨
    // playerMap, monsterMap, uidToAccount 등 게임 상태는 이 strand에 의해 보호됨
    boost::asio::io_context::strand game_strand_;

    // [수정] playerMutex_, monsterMutex_ 제거 — game_strand_가 직렬화를 담당
    std::unordered_map<std::string, std::shared_ptr<PlayerInfo>> playerMap;
    std::unordered_map<uint64_t, std::string> uidToAccount;

    std::unordered_map<uint64_t, std::shared_ptr<Monster>> monsterMap;
    std::vector<std::shared_ptr<Monster>> monsters;

    // [atomic] 락 없이 안전한 UID 발급
    std::atomic<uint64_t> uidCounter{ 1 };

    boost::asio::io_context ai_io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> ai_work_guard;

    PacketDispatcher<GatewaySession> gatewayDispatcher;
    PacketDispatcher<WorldConnection> worldDispatcher;

    std::shared_ptr<WorldConnection> worldConnection;
    std::unordered_set<std::shared_ptr<GatewaySession>> gatewaySessions;
    std::mutex gatewaySessionMutex;

    std::unique_ptr<Zone> zone;
    NavMesh navMesh;

    // ==========================================
    // [추가] 게이트웨이 장애 복구용 유저 소속 추적
    //
    // Key: GatewaySession 원시 포인터 (세션 식별용)
    // Value: 해당 게이트웨이를 통해 접속한 유저의 account_id 집합
    //
    // game_strand_ 안에서만 접근하므로 별도 뮤텍스 불필요.
    // GatewaySession 연결 해제 시 이 맵을 조회하여 유저 일괄 정리.
    // ==========================================
    std::unordered_map<GatewaySession*, std::unordered_set<std::string>> gatewayPlayerMap_;

    // 유저를 게이트웨이 소속으로 등록 (game_strand_ 안에서 호출)
    void RegisterPlayerToGateway(GatewaySession* gw, const std::string& account_id) {
        gatewayPlayerMap_[gw].insert(account_id);
    }

    // 유저를 게이트웨이 소속에서 제거 (game_strand_ 안에서 호출)
    void UnregisterPlayerFromGateway(GatewaySession* gw, const std::string& account_id) {
        auto it = gatewayPlayerMap_.find(gw);
        if (it != gatewayPlayerMap_.end()) {
            it->second.erase(account_id);
            if (it->second.empty()) gatewayPlayerMap_.erase(it);
        }
    }

    // 게이트웨이에 소속된 전체 유저 목록 반환 (game_strand_ 안에서 호출)
    std::vector<std::string> GetPlayersOfGateway(GatewaySession* gw) const {
        auto it = gatewayPlayerMap_.find(gw);
        if (it == gatewayPlayerMap_.end()) return {};
        return std::vector<std::string>(it->second.begin(), it->second.end());
    }

    // 게이트웨이 소속 매핑 전체 삭제 (game_strand_ 안에서 호출)
    void RemoveGatewayMapping(GatewaySession* gw) {
        gatewayPlayerMap_.erase(gw);
    }

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
    std::atomic<uint64_t> processed_packet_count{ 0 };
    std::atomic<int> connected_bot_count{ 0 };
#endif

    std::vector<std::thread> managed_threads_;
    std::atomic<bool> is_running_{ true };

    inline static GameContext* s_test_instance_ = nullptr;

    static GameContext& Get() {
        if (s_test_instance_) return *s_test_instance_;
        static GameContext instance;
        return instance;
    }

    static void SetTestInstance(GameContext* instance) noexcept {
        s_test_instance_ = instance;
    }

    void AddManagedThread(std::thread t) {
        managed_threads_.push_back(std::move(t));
    }

    void Shutdown() {
        is_running_.store(false);
        ai_work_guard.reset();
        ai_io_context.stop();
        for (auto& t : managed_threads_) {
            if (t.joinable()) t.join();
        }
        managed_threads_.clear();
        std::cout << "[GameContext] Shutdown 완료: 모든 관리 스레드 종료됨.\n";
    }

    void BroadcastToGateways(uint16_t pktId, const google::protobuf::Message& msg);

    // [수정] game_strand_ 초기화 추가
    GameContext()
        : ai_work_guard(boost::asio::make_work_guard(ai_io_context))
        , game_strand_(io_context) {
    }

    GameContext(const GameContext&) = delete;
    GameContext& operator=(const GameContext&) = delete;
};

extern thread_local dtNavMeshQuery* t_navQuery;
