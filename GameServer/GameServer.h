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
#include "../Common/Utils/Lock.h"

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
// GameContext 싱글톤 + 의존성 주입(DI) 지원
//
// [락 설계 개선]
// 변경 전: 단일 shared_mutex(gameStateMutex)로 playerMap, uidToAccount,
//          monsterMap을 모두 보호 -> 몬스터 AI가 플레이어 이동을 차단
//
// 변경 후: 역할별 락 분리
//   playerMutex_  - playerMap + uidToAccount 보호
//   monsterMutex_ - monsterMap + monsters 보호 (초기화 이후 거의 읽기 전용)
//   uidCounter    - atomic으로 변경 (락 불필요)
//
// 효과: 몬스터 AI Tick이 플레이어 이동을 차단하지 않음.
//       서로 다른 자원에 대한 접근이 독립적으로 동작.
// ==========================================
struct GameContext {
    DataManager dataManager;

    boost::asio::io_context io_context;

    // [락 분리] 플레이어 상태 보호 (이동, 접속/퇴장 빈번)
    // [수정] UTILITY::Lock(std::shared_timed_mutex) 타입으로 통일
    mutable UTILITY::Lock playerMutex_;
    std::unordered_map<std::string, std::shared_ptr<PlayerInfo>> playerMap;
    std::unordered_map<uint64_t, std::string> uidToAccount;

    // [락 분리] 몬스터 상태 보호 (초기화 후 거의 읽기 전용)
    // [수정] UTILITY::Lock(std::shared_timed_mutex) 타입으로 통일
    mutable UTILITY::Lock monsterMutex_;
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

    GameContext()
        : ai_work_guard(boost::asio::make_work_guard(ai_io_context)) {
    }

    GameContext(const GameContext&) = delete;
    GameContext& operator=(const GameContext&) = delete;
};

extern thread_local dtNavMeshQuery* t_navQuery;
