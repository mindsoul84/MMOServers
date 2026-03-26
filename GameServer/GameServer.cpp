#include "GameServer.h"
#include "Session/GatewaySession.h"
#include "Network/WorldConnection.h"
#include "Handlers/GatewayGame/GatewayHandlers.h"
#include "Handlers/WorldGame/WorldHandlers.h"

#include "../Common/ConfigManager.h"
#include "../Common/DB/DBManager.h"
#include "../Common/DataManager/DataManager.h"
#include "../Common/Utils/Logger.h"
#include "Pathfinder/MapGenerator.h"
#include "Monster/MonsterManager.h"

#include <iostream>
#include <thread>
#include <windows.h>
#include <csignal>

#include <recastnavigation/DetourNavMesh.h>
#include <recastnavigation/DetourNavMeshBuilder.h>
#include <recastnavigation/DetourNavMeshQuery.h>

using boost::asio::ip::tcp;

thread_local dtNavMeshQuery* t_navQuery = nullptr;

// ==========================================
// Graceful Shutdown을 위한 시그널 핸들러 추가
//
// 기존 문제: io_context.run()이 자연 종료만 대기, SIGTERM 무시
// 수정: Ctrl+C(SIGINT) 수신 시 io_context.stop() → 워커 스레드 종료
// ==========================================
static boost::asio::io_context* g_main_io_context = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        LOG_INFO("System", "종료 신호 수신. Graceful Shutdown 시작...");
        if (g_main_io_context) {
            g_main_io_context->stop();
        }
        auto& ctx = GameContext::Get();
        ctx.is_running_.store(false);
        return TRUE;
    }
    return FALSE;
}

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
                LOG_INFO("GameServer", "S2S 통신: GatewayServer 접속 확인 완료!");
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

void StartAIThreadPool(int ai_thread_count) {
    LOG_INFO("System", "AI 전용 비동기 스레드 풀 가동 (" << ai_thread_count << "개)...");
    auto& ctx = GameContext::Get();

    for (int i = 0; i < ai_thread_count; ++i) {
        ctx.AddManagedThread(std::thread([i, &ctx]() {
            t_navQuery = dtAllocNavMeshQuery();
            if (t_navQuery) {
                dtStatus status = t_navQuery->init(ctx.navMesh.GetRawNavMesh(), 2048);
                if (dtStatusFailed(status)) {
                    LOG_ERROR("AI", "Thread " << i << " NavMeshQuery 초기화 실패");
                }
            }

            ctx.ai_io_context.run();

            if (t_navQuery) {
                dtFreeNavMeshQuery(t_navQuery);
                t_navQuery = nullptr;
            }
            LOG_INFO("AI", "Thread " << i << " 정상 종료됨.");
        }));
    }
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // 시그널 핸들러 등록
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\GameServer_Unique_Mutex_Lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_FATAL("Error", "GameServer가 이미 실행 중입니다. 창을 닫습니다.");
        CloseHandle(hMutex);
        return 1;
    }

    if (!ConfigManager::GetInstance().LoadConfig("config.json")) {
        LOG_FATAL("System", "config 설정 파일 오류로 인해 GameServer 종료합니다.");
        system("pause");
        return -1;
    }

    // raw new → unique_ptr (make_unique 사용)
    if (ConfigManager::GetInstance().UseDB()) {
        t_dbManager = std::make_unique<DBManager>();
        if (!t_dbManager->Connect()) {
            LOG_FATAL("System", "DB 연결에 실패하여 서버를 종료합니다.");
            return -1;
        }
    }
    else {
        LOG_WARN("System", "config.json 설정에 따라 DB 연동을 건너뜁니다.");
    }

    auto& ctx = GameContext::Get();

    if (!ctx.dataManager.LoadAllData("JsonData/")) {
        LOG_FATAL("System", "몬스터 데이터를 불러오지 못해 서버를 종료합니다.");
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
        LOG_INFO("System", "코어 게임 로직 서버 가동 (Port: " << game_port << ") Created by Jeong Shin Young");

        // 시그널 핸들러에서 io_context를 정지시킬 수 있도록 포인터 저장
        g_main_io_context = &ctx.io_context;

        ctx.worldConnection = std::make_shared<WorldConnection>(ctx.io_context);
        short world_port = ConfigManager::GetInstance().GetGameWorldConnPort();
        ctx.worldConnection->Connect("127.0.0.1", world_port);

        unsigned int max_thread_count = ConfigManager::GetInstance().GetGameMaxThreadCount();
        if (max_thread_count == 0) max_thread_count = std::thread::hardware_concurrency();
        LOG_INFO("System", "워커 스레드 개수 설정: " << max_thread_count << "개");

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
        ctx.AddManagedThread(std::thread([]() {
            uint64_t last_count = 0;
            auto& ctx = GameContext::Get();

            while (ctx.is_running_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!ctx.is_running_.load()) break;

                uint64_t current_count = ctx.processed_packet_count.load();
                int bot_count = ctx.connected_bot_count.load();

                if (bot_count > 0 && current_count == last_count) {
                    LOG_FATAL("Watchdog", "5초간 처리된 패킷이 0개입니다! 데드락 발생 의심!!!");
                }
                else if (bot_count > 0) {
                    LOG_INFO("Watchdog", "서버 정상 틱 동작 중 (5초간 처리량: " << (current_count - last_count) << " pkts)");
                }
                last_count = current_count;
            }
            LOG_INFO("Watchdog", "정상 종료됨.");
        }));
#endif

        LOG_INFO("System", "스레드 풀 구성 중...");
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < max_thread_count; ++i) {
            threads.emplace_back([&ctx]() {
                ctx.io_context.run();
            });
        }
        LOG_INFO("System", "스레드 풀 구성 완료. GatewayServer S2S 접속 대기 중...");

        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }
    catch (std::exception& e) {
        LOG_FATAL("Error", "예외 발생: " << e.what());
    }

    // ★ Graceful Shutdown
    ctx.Shutdown();

    g_main_io_context = nullptr;
    LOG_INFO("System", "서버가 안전하게 종료되었습니다.");
    return 0;
}
