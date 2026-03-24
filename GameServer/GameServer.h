#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
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

struct PlayerInfo {
    uint64_t uid;
    float x, y;
    int hp  = GameConstants::Player::DEFAULT_HP;
    int atk = GameConstants::Player::DEFAULT_ATK;
    int def = GameConstants::Player::DEFAULT_DEF;

    std::mutex mtx;

    PlayerInfo() = default;
    PlayerInfo(const PlayerInfo&) = delete;
    PlayerInfo& operator=(const PlayerInfo&) = delete;
};

// ==========================================
// ★ [수정 1] GameContext 싱글톤 → 의존성 주입(DI) 지원
//
// 변경 전: private 생성자 + static Get() → 테스트 불가
// 변경 후: public 생성자 + SetTestInstance() → 테스트에서 목(mock) 주입 가능
//
// ★ [수정 4] thread::detach 남용 → managed_threads_ 로 안전한 종료 지원
//   - AddManagedThread()로 등록된 스레드는 Shutdown()에서 모두 join()됨
//   - graceful shutdown: Shutdown() 호출 → ai_io_context/io_context 정지 → join
// ==========================================
struct GameContext {
    DataManager dataManager;

    boost::asio::io_context io_context;

    mutable std::shared_mutex gameStateMutex;

    boost::asio::io_context ai_io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> ai_work_guard;

    PacketDispatcher<GatewaySession> gatewayDispatcher;
    PacketDispatcher<WorldConnection> worldDispatcher;

    std::shared_ptr<WorldConnection> worldConnection;
    std::unordered_set<std::shared_ptr<GatewaySession>> gatewaySessions;
    std::mutex gatewaySessionMutex;

    std::unique_ptr<Zone> zone;
    NavMesh navMesh;
    std::vector<std::shared_ptr<Monster>> monsters;

    std::unordered_map<std::string, std::shared_ptr<PlayerInfo>> playerMap;
    std::unordered_map<uint64_t, std::shared_ptr<Monster>> monsterMap;
    std::unordered_map<uint64_t, std::string> uidToAccount;
    uint64_t uidCounter = 1;

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
    std::atomic<uint64_t> processed_packet_count{ 0 };
    std::atomic<int> connected_bot_count{ 0 };
#endif

    // ★ [추가 - 수정 4] detach 대신 managed_threads_ 에 보관 → Shutdown()에서 join
    std::vector<std::thread> managed_threads_;

    // ★ [추가 - 수정 4] 정상 종료 신호 플래그
    std::atomic<bool> is_running_{ true };

    // ★ [수정 1] 테스트 인스턴스 오버라이드 포인터
    // ★ [수정] inline 정의 (C++17) → GameServer.cpp 없이도 링크 완결
    inline static GameContext* s_test_instance_ = nullptr;

    // ★ [수정 1] 테스트 인스턴스가 주입된 경우 반환, 없으면 정적 싱글톤 반환
    static GameContext& Get() {
        if (s_test_instance_) return *s_test_instance_;
        static GameContext instance;
        return instance;
    }

    // ★ [추가 - 수정 1] 테스트 전용 주입 메서드
    static void SetTestInstance(GameContext* instance) noexcept {
        s_test_instance_ = instance;
    }

    // ★ [추가 - 수정 4] detached 스레드 대신 사용. 스레드를 managed_threads_에 등록
    void AddManagedThread(std::thread t) {
        managed_threads_.push_back(std::move(t));
    }

    // ★ [추가 - 수정 4] Graceful Shutdown:
    //   1) is_running_ = false 로 루프 종료 신호
    //   2) ai_io_context 정지 → AI 스레드 종료
    //   3) 모든 managed_threads_ join
    void Shutdown() {
        is_running_.store(false);
        ai_work_guard.reset();          // work_guard 해제 → run() 이 반환될 수 있게
        ai_io_context.stop();           // AI 전용 스레드 풀 종료
        for (auto& t : managed_threads_) {
            if (t.joinable()) t.join();
        }
        managed_threads_.clear();
        std::cout << "[GameContext] Shutdown 완료: 모든 관리 스레드 종료됨.\n";
    }

    void BroadcastToGateways(uint16_t pktId, const google::protobuf::Message& msg);

    // ★ [수정 1] public 생성자 → 테스트 코드에서 GameContext ctx; 로 직접 생성 가능
    GameContext()
        : ai_work_guard(boost::asio::make_work_guard(ai_io_context)) {
    }

    // 복사/이동 금지
    GameContext(const GameContext&) = delete;
    GameContext& operator=(const GameContext&) = delete;
};

extern thread_local dtNavMeshQuery* t_navQuery;
