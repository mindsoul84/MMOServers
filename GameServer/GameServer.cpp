#include "GameServer.h"
#include "Session/GatewaySession.h"
#include "Network/WorldConnection.h"
#include "Handlers/GatewayGame/GatewayHandlers.h"
#include "Handlers/WorldGame/WorldHandlers.h"

#include "../Common/ConfigManager.h"
#include "../Common/DB/DBManager.h"
#include "../Common/DataManager/DataManager.h"
#include "../Common/ConfigManager.h"
#include "Pathfinder/MapGenerator.h"
#include "Monster/MonsterManager.h"

#include <iostream>
#include <thread>
#include <windows.h>

#include <recastnavigation/DetourNavMesh.h>         // 핵심 구조체 정의 포함
#include <recastnavigation/DetourNavMeshBuilder.h>
#include <recastnavigation/DetourNavMeshQuery.h>

using boost::asio::ip::tcp;

// ★ Thread-Local 변수 정의
thread_local dtNavMeshQuery* t_navQuery = nullptr;

// ==========================================
// GameNetworkServer: 9000번 포트에서 Gateway의 접속(Accept) 대기
// ==========================================
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

// ==========================================
// GameContext 브로드캐스트 구현부
// ==========================================
void GameContext::BroadcastToGateways(uint16_t pktId, const google::protobuf::Message& msg) {
    std::lock_guard<std::mutex> lock(gatewaySessionMutex);
    for (auto& session : gatewaySessions) {
        if (session) {
            session->Send(pktId, msg);
        }
    }
}

void StartAIThreadPool(int ai_thread_count) {
    std::cout << "[System] 🧠 AI 전용 비동기 스레드 풀 가동 (" << ai_thread_count << "개)...\n";
    auto& ctx = GameContext::Get();

    for (int i = 0; i < ai_thread_count; ++i) {
        std::thread([i, &ctx]() {
            t_navQuery = dtAllocNavMeshQuery();
            dtStatus status = t_navQuery->init(ctx.navMesh.GetRawNavMesh(), 2048);

            ctx.ai_io_context.run(); // 무한 대기

            if (t_navQuery) {
                dtFreeNavMeshQuery(t_navQuery);
                t_navQuery = nullptr;
            }
            }).detach();
    }
}

// ==========================================
// 4. 메인 함수: 스레드 풀 구성 및 서버 실행
// ==========================================
int main() {

    // 윈도우 콘솔 한글 깨짐 방지
    SetConsoleOutputCP(CP_UTF8);

    // =========================================================
    // ★ [중복 실행 방지] 고유한 이름의 Named Mutex 생성
    // =========================================================
    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\GameServer_Unique_Mutex_Lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "🚨 [Error] GameServer가 이미 실행 중입니다. 창을 닫습니다.\n";
        CloseHandle(hMutex);
        return 1;
    }
    // =========================================================

    // 1. 가장 먼저 환경 설정(config.json)을 로드합니다.
    if (!ConfigManager::GetInstance().LoadConfig("config.json"))
    {
        std::cerr << "🚨 config 설정 파일 오류로 인해 GameServer 종료합니다.\n";
        system("pause"); // 디버깅 창이 바로 꺼지지 않게 대기
        return -1;
    }

    // 2. 설정에 DB 연동이 true로 되어 있다면 DB 연결 시도
    if (ConfigManager::GetInstance().UseDB()) {        
        // =========================================================
        // ★ [수정] GetInstance() 대신 현재 스레드의 t_dbManager를 생성하고 연결합니다.
        // =========================================================
        t_dbManager = new DBManager();

        if (!t_dbManager->Connect()) {
            std::cerr << "DB 연결에 실패하여 서버를 종료합니다.\n";
            return -1;
        }
    }
    else {
        std::cout << "[System] ⚠️ config.json 설정에 따라 DB 연동을 건너뜁니다.\n";
    }

    // 추가: 한글 세팅이 끝난 안전한 타이밍에 Zone을 생성합니다!
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

    // [추가] 파일이 없으면 즉석에서 만들어주는 제너레이터 가동!
    GenerateDummyMapFile("dummy_map.bin");

    // ---------------------------------------------------------
    // 맵 데이터 로드 및 몬스터 스폰
    // ---------------------------------------------------------
    ctx.navMesh.LoadNavMeshFromFile("dummy_map.bin");

    // 3. 몬스터 스폰 및 AI 시스템 가동 (분리된 모듈 호출)
    InitMonsters();
    StartAITickThread();
    // =========================================================
    short ai_thread_count = ConfigManager::GetInstance().GetGameAiThreadCount();
    StartAIThreadPool(ai_thread_count);   // 길찾기 전담 스레드 풀(4개) 가동
    // =========================================================
    
    // ★ 디스패처 등록
    ctx.gatewayDispatcher.RegisterHandler(Protocol::PKT_GATEWAY_GAME_MOVE_REQ, Handle_GatewayGameMoveReq);
    ctx.gatewayDispatcher.RegisterHandler(Protocol::PKT_GATEWAY_GAME_LEAVE_REQ, Handle_GatewayGameLeaveReq);
    ctx.gatewayDispatcher.RegisterHandler(Protocol::PKT_GATEWAY_GAME_ATTACK_REQ, Handle_GatewayGameAttackReq);

    ctx.worldDispatcher.RegisterHandler(Protocol::PKT_WORLD_GAME_MONSTER_BUFF, Handle_WorldGameMonsterBuff);
    
    try {
        //boost::asio::io_context io_context;

        // S2S 서버 객체 생성 (포트: 9000)
        short game_port = ConfigManager::GetInstance().GetGameServerPort();
        GameNetworkServer server(ctx.io_context, game_port);
        std::cout << "[System] 코어 게임 로직 서버 가동 (Port: " << game_port << ") Created by Jeong Shin Young\n";

        // =========================================================
        // WorldServer 연결
        // =========================================================
        ctx.worldConnection = std::make_shared<WorldConnection>(ctx.io_context);
        short world_port = ConfigManager::GetInstance().GetGameWorldConnPort();
        ctx.worldConnection->Connect("127.0.0.1", world_port);
        // =========================================================

        // CPU 코어 개수에 맞춰 스레드 개수 설정
        unsigned int max_thread_count = ConfigManager::GetInstance().GetGameMaxThreadCount();
        if (max_thread_count == 0) max_thread_count = std::thread::hardware_concurrency();
        std::cout << "[System] 워커 스레드 개수 설정: " << max_thread_count << "개\n";

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
        // =========================================================
        // 워치독(Watchdog) 데드락 감시 스레드
        // =========================================================
        std::thread watchdog_thread([]() {
            uint64_t last_count = 0;

            while (true) {
                // 5초마다 깨어나서 서버 상태를 검사합니다.
                std::this_thread::sleep_for(std::chrono::seconds(5));

                auto& ctx = GameContext::Get();
                uint64_t current_count = ctx.processed_packet_count.load();
                int bot_count = ctx.connected_bot_count.load(); // 봇 카운트 가져오기

                // 유저(봇)가 1명이라도 접속해 있는데, 5초 동안 처리한 패킷이 0개라면?!
                if (bot_count > 0 && current_count == last_count) {
                    std::cerr << "\n🚨🚨 [Watchdog] FATAL ERROR: 5초간 처리된 패킷이 0개입니다! 서버 스레드 데드락(Deadlock) 발생!!! 🚨🚨\n\n";
                }
                else if (bot_count > 0) {
                    // 정상 작동 중일 때의 TPS(5초간) 출력
                    std::cout << "⏱️ [Watchdog] 서버 정상 틱 동작 중 (5초간 처리량: "
                        << (current_count - last_count) << " pkts)\n";
                }

                last_count = current_count;
            }
            });
        watchdog_thread.detach(); // 백그라운드에서 영원히 돌게 둡니다.
        // =========================================================
#endif//DEF_STRESS_TEST_DEADLOCK_WATCHDOG

        // 스레드 풀 생성 및 io_context.run() 실행
        std::cout << "[System] 여러 스레드에서 io_context.run()을 호출하여 스레드 풀을 구성합니다...\n";
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < max_thread_count; ++i) {
            threads.emplace_back([&ctx]() {
                ctx.io_context.run();
            });
        }
        std::cout << "[System] 스레드 풀 구성 완료.\n";

        // 4. 메인 스레드 대기 (join)
        std::cout << "=================================================\n";
        std::cout << "[System] GatewayServer의 S2S 접속을 기다리는 중...\n";

        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    catch (std::exception& e) {
        std::cerr << "[Error] 예외 발생: " << e.what() << "\n";
    }

    std::cout << "[System] 서버가 안전하게 종료되었습니다.\n";
    return 0;
}