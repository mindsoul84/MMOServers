#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex> // Read-Write Lock을 위한 헤더

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "PacketDispatcher.h"
#include "Zone/Zone.h"
#include "Monster/Monster.h"
#include "Pathfinder/Pathfinder.h"

#include "../Common/DataManager/DataManager.h"

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

class GatewaySession;
class WorldConnection;

// ==========================================
// ★ 유저 정보 구조체
// ==========================================
struct PlayerInfo {
    uint64_t uid;
    float x, y;
    int hp = 100;
    int atk = 30;
    int def = 5;

    // ☆ 유저 개별 락 (Fine-grained Lock) ★
    std::mutex mtx; 

    PlayerInfo() = default;

    // mutex는 복사/이동이 불가능하므로 삭제 처리
    PlayerInfo(const PlayerInfo&) = delete;
    PlayerInfo& operator=(const PlayerInfo&) = delete;
};

// ==========================================
// ★ [리팩토링] GameServer의 모든 상태를 관리하는 단일 Context
// ==========================================
struct GameContext {
    // =========================================================
    // ★ 기존 매니저 Context 내부로 편입
    // =========================================================
    DataManager dataManager;

    // 1. 코어 네트워크 및 메인 게임 스레드 큐
    boost::asio::io_context io_context;

    // ★ [핵심] Lock 모델 도입: 플레이어 맵과 몬스터 맵 등을 보호할 중앙 상태 락(Read-Write Lock)
    mutable std::shared_mutex gameStateMutex;

    // 2. AI 전용 비동기 길찾기 큐
    boost::asio::io_context ai_io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> ai_work_guard;

    // 3. 디스패처 (게이트웨이 방향 / 월드 방향)
    PacketDispatcher<GatewaySession> gatewayDispatcher;
    PacketDispatcher<WorldConnection> worldDispatcher;

    // 4. S2S 커넥션 관리
    std::shared_ptr<WorldConnection> worldConnection;
    std::unordered_set<std::shared_ptr<GatewaySession>> gatewaySessions;
    std::mutex gatewaySessionMutex;

    // 5. 인게임 월드 상태 데이터
    std::unique_ptr<Zone> zone;
    NavMesh navMesh;
    std::vector<std::shared_ptr<Monster>> monsters;

    std::unordered_map<std::string, std::shared_ptr<PlayerInfo>> playerMap;  // account_id -> PlayerInfo
    std::unordered_map<uint64_t, std::shared_ptr<Monster>> monsterMap;

    std::unordered_map<uint64_t, std::string> uidToAccount; // uid -> account_id
    uint64_t uidCounter = 1;

    // 싱글톤
    static GameContext& Get() {
        static GameContext instance;
        return instance;
    }

    // ★ 브로드캐스트 헬퍼 함수
    void BroadcastToGateways(uint16_t pktId, const google::protobuf::Message& msg);

private:
    GameContext()
        : ai_work_guard(boost::asio::make_work_guard(ai_io_context)) {
    }
};

// ★ 길찾기용 Thread-Local 객체는 스레드별로 할당되어야 하므로 extern 유지
extern thread_local dtNavMeshQuery* t_navQuery;