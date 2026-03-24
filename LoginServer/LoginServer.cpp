
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

// ★ 정적 멤버 정의 (싱글톤 DI 오버라이드 포인터)
// s_test_instance_ 는 헤더에서 inline 으로 정의됩니다. (중복 정의 제거)

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
                    LoginContext::Get().connected_clients++;
                    std::make_shared<Session>(std::move(socket))->start();
                }
                do_accept();
                });
        }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\LoginServer_Unique_Mutex_Lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "🚨 [Error] LoginServer가 이미 실행 중입니다. 창을 닫습니다.\n";
        CloseHandle(hMutex);
        return 1;
    }

    if (!ConfigManager::GetInstance().LoadConfig("config.json")) {
        std::cerr << "🚨 config 설정 파일 오류로 인해 LoginServer 종료합니다.\n";
        system("pause");
        return -1;
    }

    auto& ctx = LoginContext::Get();

    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_LOGIN_LOGIN_REQ,        Handle_LoginReq);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_SERVER_HEARTBEAT,       Handle_Heartbeat);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_LOGIN_WORLD_SELECT_REQ, Handle_WorldSelectReq);
    ctx.worldDispatcher.RegisterHandler(Protocol::PKT_WORLD_LOGIN_SELECT_RES,         Handle_S2SWorldSelectRes);

    try {
        boost::asio::io_context io_context;

        ctx.worldConnection = std::make_shared<WorldConnection>(std::ref(io_context));
        short world_port = ConfigManager::GetInstance().GetLoginWorldConnPort();
        ctx.worldConnection->Connect("127.0.0.1", world_port);

        short login_port = ConfigManager::GetInstance().GetLoginServerPort();
        LoginServer server(io_context, login_port);
        std::cout << "[LoginServer] 로그인 서버 가동 시작 (Port: " << login_port << ") Created by Jeong Shin Young\n";
        std::cout << "=================================================\n";

        // ==========================================
        // ★ [수정 4] DB 전담 스레드: detach() → db_threads_ 벡터에 보관
        //
        // 변경 전: std::thread(...).detach() → Shutdown 불가, 리소스 누수 위험
        // 변경 후: ctx.db_threads_.emplace_back(...) → Shutdown()에서 join() 가능
        // ==========================================
        int db_thread_count = ConfigManager::GetInstance().GetLoginDbThreadCount();
        std::cout << "[System] 💾 DB 연산 전용 백그라운드 스레드 가동 (" << db_thread_count << "개)...\n";

        for (int i = 0; i < db_thread_count; ++i) {
            // ★ [수정 4] detach() 제거 → db_threads_ 벡터에 push
            ctx.db_threads_.emplace_back([i, &ctx]() {
                if (ConfigManager::GetInstance().UseDB()) {
                    t_dbManager = new DBManager();
                    if (!t_dbManager->Connect()) {
                        std::cerr << "🚨 [DB 스레드 " << i << "] DB 연결 실패!\n";
                    }
                }

                // db_io_context.stop() 호출 시 run()이 반환되어 스레드 종료
                ctx.db_io_context.run();

                if (t_dbManager) {
                    delete t_dbManager;
                    t_dbManager = nullptr;
                }
                std::cout << "[DB Thread " << i << "] 정상 종료됨.\n";
            });
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

    // ★ [수정 4] Graceful Shutdown: DB 스레드 모두 join
    ctx.Shutdown();

    return 0;
}
