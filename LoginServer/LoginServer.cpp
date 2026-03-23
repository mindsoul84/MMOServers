
#include "LoginServer.h"
#include "Session/Session.h"
#include "Network/WorldConnection.h"
#include "Handlers/ClientLogin/ClientHandlers.h"
#include "Handlers/LoginWorld/WorldHandlers.h"

#include "..\Common\ConfigManager.h"
#include "..\Common\DB\DBManager.h"
#include "..\Common\MemoryPool.h"

#include <iostream>
#include <windows.h>
#include <thread>

using boost::asio::ip::tcp;

// ==========================================
// ★ 전역 메모리 할당
// ==========================================
boost::asio::io_context g_db_io_context;                              // ★ DB 전용 컨텍스트
auto g_db_work_guard = boost::asio::make_work_guard(g_db_io_context); // ★ 종료 방지 가드

std::atomic<int> g_connected_clients{ 0 };
std::unordered_set<std::string> g_loggedInUsers;
std::unordered_map<std::string, std::shared_ptr<Session>> g_sessionMap;
std::mutex g_loginMutex;

PacketDispatcher<Session> g_client_dispatcher;
PacketDispatcher<WorldConnection> g_world_dispatcher;
std::shared_ptr<WorldConnection> g_worldConnection; // 전역 S2S 커넥션 포인터

// ==========================================
// Server 대기열 및 Main
// ==========================================
class LoginServer {
    private:
        tcp::acceptor acceptor_;
    public:
        LoginServer(boost::asio::io_context& io_context, short port)
            : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
            do_accept();
        }
    private:
        void do_accept() {
            acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    g_connected_clients++;
                    std::make_shared<Session>(std::move(socket))->start();
                }
                do_accept();
                });
        }
};

int main() {    
    
    SetConsoleOutputCP(CP_UTF8);

    // =========================================================
    // ★ [중복 실행 방지] 고유한 이름의 Named Mutex 생성
    // =========================================================
    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\LoginServer_Unique_Mutex_Lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "🚨 [Error] LoginServer가 이미 실행 중입니다. 창을 닫습니다.\n";
        CloseHandle(hMutex);
        return 1;
    }
    // =========================================================

    if (!ConfigManager::GetInstance().LoadConfig("config.json"))
    {
        std::cerr << "🚨 config 설정 파일 오류로 인해 LoginServer 종료합니다.\n";
        system("pause"); // 디버깅 창이 바로 꺼지지 않게 대기
        return -1;
    }

    // ★ 분리된 핸들러들을 디스패처에 등록
    g_client_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_LOGIN_LOGIN_REQ, Handle_LoginReq);
    g_client_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_SERVER_HEARTBEAT, Handle_Heartbeat);
    g_client_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_LOGIN_WORLD_SELECT_REQ, Handle_WorldSelectReq);
    g_world_dispatcher.RegisterHandler(Protocol::PKT_WORLD_LOGIN_SELECT_RES, Handle_S2SWorldSelectRes);

    try {
        boost::asio::io_context io_context;

        g_worldConnection = std::make_shared<WorldConnection>(std::ref(io_context));
        short world_port = ConfigManager::GetInstance().GetLoginWorldConnPort();
        g_worldConnection->Connect("127.0.0.1", world_port);

        short login_port = ConfigManager::GetInstance().GetLoginServerPort();
        LoginServer server(io_context, login_port);
        std::cout << "[LoginServer] 로그인 서버 가동 시작 (Port: " << login_port << ") Created by Jeong Shin Young\n";
        std::cout << "=================================================\n";

        // =========================================================
        // ★ DB 전담 워커 스레드 구동 (Thread-Local DB 연결)
        // =========================================================
        int db_thread_count = ConfigManager::GetInstance().GetLoginDbThreadCount();

        std::cout << "[System] 💾 DB 연산 전용 백그라운드 스레드 가동 (" << db_thread_count << "개)...\n";
        for (int i = 0; i < db_thread_count; ++i) {
            std::thread([i]() {
                // 1. 이 스레드만의 전용 DB 연결 객체 생성
                if (ConfigManager::GetInstance().UseDB()) {
                    t_dbManager = new DBManager();
                    if (!t_dbManager->Connect()) {
                        std::cerr << "🚨 [DB 스레드 " << i << "] DB 연결 실패!\n";
                    }
                }

                // 2. 무한 대기하며 큐에 들어오는 로그인 요청(Job) 처리
                g_db_io_context.run();

                // 3. 스레드가 종료될 때 안전하게 메모리 해제
                if (t_dbManager) {
                    delete t_dbManager;
                    t_dbManager = nullptr;
                }
                }).detach();
        }
        std::cout << "=================================================\n";

        unsigned int max_thread_count = ConfigManager::GetInstance().GetLoginMaxThreadCount();
        if (max_thread_count == 0) max_thread_count = std::thread::hardware_concurrency();
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < max_thread_count; ++i) {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }
        for (auto& t : threads) { if (t.joinable()) t.join(); }
    }
    catch (std::exception& e) {
        std::cerr << "[Error] 서버 예외 발생: " << e.what() << "\n";
    }
    return 0;
}