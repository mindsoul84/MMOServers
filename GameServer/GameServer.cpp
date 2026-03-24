#include "GameServer.h"
#include "Session/GatewaySession.h"
#include "Network/WorldConnection.h"
#include "Handlers/GatewayGame/GatewayHandlers.h"
#include "Handlers/WorldGame/WorldHandlers.h"

#include "../Common/ConfigManager.h"
#include "../Common/DB/DBManager.h"
#include "../Common/DataManager/DataManager.h"
#include "Pathfinder/MapGenerator.h"
#include "Monster/MonsterManager.h"

#include <iostream>
#include <thread>
#include <windows.h>

#include <recastnavigation/DetourNavMesh.h>
#include <recastnavigation/DetourNavMeshBuilder.h>
#include <recastnavigation/DetourNavMeshQuery.h>

using boost::asio::ip::tcp;

// ★ 정적 멤버 정의 (싱글톤 DI 오버라이드 포인터)
// s_test_instance_ 는 헤더에서 inline 으로 정의됩니다. (중복 정의 제거)

thread_local dtNavMeshQuery* t_navQuery = nullptr;

class GameNetworkServer {
    tcp::acceptor acceptor_;
public:
    GameNetworkServer(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }
private:
    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::cout << "[GameServer] S2S 통신: GatewayServer 접속 확인 완료!\n";
                auto new_session = std::make_shared<GatewaySession>(std::move(socket));
                {
                    std::lock_guard<std::mutex> lock(GameContext::Get().gatewaySessionMutex);
                    GameContext::Get().gatewaySessions.insert(new_session);
                }
                new_session->start();
            }
            do_accept();
            });
    }
};

void GameContext::BroadcastToGateways(uint16_t pktId, const google::protobuf::Message& msg) {
    std::lock_guard<std::mutex> lock(gatewaySessionMutex);
    for (auto& session : gatewaySessions) {
        if (session) {
            session->Send(pktId, msg);
        }
    }
}

// ==========================================
// ★ [수정 4] thread::detach 제거 → AddManagedThread()로 안전하게 관리
//
// 변경 전: std::thread(...).detach() → graceful shutdown 불가능
// 변경 후: ctx.AddManagedThread(std::thread(...)) → Shutdown()에서 join 가능
// ==========================================
void StartAIThreadPool(int ai_thread_count) {
    std::cout << "[System] 🧠 AI 전용 비동기 스레드 풀 가동 (" << ai_thread_count << "개)...\n";
    auto& ctx = GameContext::Get();

    for (int i = 0; i < ai_thread_count; ++i) {
        // ★ [수정 4] detach() → AddManagedThread(): Shutdown()에서 join() 가능
        ctx.AddManagedThread(std::thread([i, &ctx]() {
            t_navQuery = dtAllocNavMeshQuery();
            if (t_navQuery) {
                dtStatus status = t_navQuery->init(ctx.navMesh.GetRawNavMesh(), 2048);
                if (dtStatusFailed(status)) {
                    std::cerr << "[AI Thread " << i << "] NavMeshQuery 초기화 실패\n";
                }
            }

            ctx.ai_io_context.run(); // is_running_ == false 이거나 stop() 호출 시 반환

            if (t_navQuery) {
                dtFreeNavMeshQuery(t_navQuery);
                t_navQuery = nullptr;
            }
            std::cout << "[AI Thread " << i << "] 정상 종료됨.\n";
        }));
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\GameServer_Unique_Mutex_Lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "🚨 [Error] GameServer가 이미 실행 중입니다. 창을 닫습니다.\n";
        CloseHandle(hMutex);
        return 1;
    }

    if (!ConfigManager::GetInstance().LoadConfig("config.json")) {
        std::cerr << "🚨 config 설정 파일 오류로 인해 GameServer 종료합니다.\n";
        system("pause");
        return -1;
    }

    if (ConfigManager::GetInstance().UseDB()) {
        t_dbManager = new DBManager();
        if (!t_dbManager->Connect()) {
            std::cerr << "DB 연결에 실패하여 서버를 종료합니다.\n";
            return -1;
        }
    }
    else {
        std::cout << "[System] ⚠️ config.json 설정에 따라 DB 연동을 건너뜁니다.\n";
    }

    auto& ctx = GameContext::Get();

    if (!ctx.dataManager.LoadAllData("JsonData/")) {
        std::cerr << "몬스터 데이터를 불러오지 못해 서버를 종료합니다.\n";
        return -1;
    }

    ctx.zone = std::make_unique<Zone>(
        static_cast<int>(GameConstants::Map::WIDTH),
        static_cast<int>(GameConstants::Map::HEIGHT),
        GameConstants::Map::SECTOR_SIZE
    );

    GenerateDummyMapFile("dummy_map.bin");
    ctx.navMesh.LoadNavMeshFromFile("dummy_map.bin");

    InitMonsters();
    StartAITickThread();

    short ai_thread_count = ConfigManager::GetInstance().GetGameAiThreadCount();
    StartAIThreadPool(ai_thread_count);

    ctx.gatewayDispatcher.RegisterHandler(Protocol::PKT_GATEWAY_GAME_MOVE_REQ,   Handle_GatewayGameMoveReq);
    ctx.gatewayDispatcher.RegisterHandler(Protocol::PKT_GATEWAY_GAME_LEAVE_REQ,  Handle_GatewayGameLeaveReq);
    ctx.gatewayDispatcher.RegisterHandler(Protocol::PKT_GATEWAY_GAME_ATTACK_REQ, Handle_GatewayGameAttackReq);
    ctx.worldDispatcher.RegisterHandler(Protocol::PKT_WORLD_GAME_MONSTER_BUFF,   Handle_WorldGameMonsterBuff);

    try {
        short game_port = ConfigManager::GetInstance().GetGameServerPort();
        GameNetworkServer server(ctx.io_context, game_port);
        std::cout << "[System] 코어 게임 로직 서버 가동 (Port: " << game_port << ") Created by Jeong Shin Young\n";

        ctx.worldConnection = std::make_shared<WorldConnection>(ctx.io_context);
        short world_port = ConfigManager::GetInstance().GetGameWorldConnPort();
        ctx.worldConnection->Connect("127.0.0.1", world_port);

        unsigned int max_thread_count = ConfigManager::GetInstance().GetGameMaxThreadCount();
        if (max_thread_count == 0) max_thread_count = std::thread::hardware_concurrency();
        std::cout << "[System] 워커 스레드 개수 설정: " << max_thread_count << "개\n";

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
        // ★ [수정 4] watchdog detach() → AddManagedThread()
        // is_running_ 플래그로 루프를 제어하여 graceful shutdown 지원
        ctx.AddManagedThread(std::thread([]() {
            uint64_t last_count = 0;
            auto& ctx = GameContext::Get();

            while (ctx.is_running_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!ctx.is_running_.load()) break; // sleep 중 shutdown 신호 수신

                uint64_t current_count = ctx.processed_packet_count.load();
                int bot_count = ctx.connected_bot_count.load();

                if (bot_count > 0 && current_count == last_count) {
                    std::cerr << "\n🚨🚨 [Watchdog] FATAL ERROR: 5초간 처리된 패킷이 0개입니다! 데드락 발생 의심!!! 🚨🚨\n\n";
                }
                else if (bot_count > 0) {
                    std::cout << "⏱️ [Watchdog] 서버 정상 틱 동작 중 (5초간 처리량: "
                        << (current_count - last_count) << " pkts)\n";
                }
                last_count = current_count;
            }
            std::cout << "[Watchdog] 정상 종료됨.\n";
        }));
#endif

        std::cout << "[System] 여러 스레드에서 io_context.run()을 호출하여 스레드 풀을 구성합니다...\n";
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < max_thread_count; ++i) {
            threads.emplace_back([&ctx]() {
                ctx.io_context.run();
            });
        }
        std::cout << "[System] 스레드 풀 구성 완료.\n";
        std::cout << "=================================================\n";
        std::cout << "[System] GatewayServer의 S2S 접속을 기다리는 중...\n";

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }
    catch (std::exception& e) {
        std::cerr << "[Error] 예외 발생: " << e.what() << "\n";
    }

    // ★ [수정 4] Graceful Shutdown: AI 스레드, watchdog 스레드 모두 join
    ctx.Shutdown();

    std::cout << "[System] 서버가 안전하게 종료되었습니다.\n";
    return 0;
}
