
#include "LoginServer.h"
#include "Session/Session.h"
#include "Network/WorldConnection.h"
#include "Handlers/ClientLogin/ClientHandlers.h"
#include "Handlers/LoginWorld/WorldHandlers.h"

#include "..\Common\ConfigManager.h"
#include "..\Common\DB\DBManager.h"
#include "..\Common\MemoryPool.h"
#include "..\Common\Utils\Logger.h"

#include <iostream>
#include <windows.h>
#include <thread>
#include <memory>

using boost::asio::ip::tcp;

// ==========================================
// Graceful Shutdown을 위한 시그널 핸들러 추가
// ==========================================
static boost::asio::io_context* g_main_io_context_login = nullptr;

static BOOL WINAPI LoginConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        LOG_INFO("System", "종료 신호 수신. Graceful Shutdown 시작...");
        if (g_main_io_context_login) {
            g_main_io_context_login->stop();
        }
        auto& ctx = LoginContext::Get();
        ctx.is_running_.store(false);
        return TRUE;
    }
    return FALSE;
}

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

    // 시그널 핸들러 등록
    SetConsoleCtrlHandler(LoginConsoleCtrlHandler, TRUE);

    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\LoginServer_Unique_Mutex_Lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_FATAL("Error", "LoginServer가 이미 실행 중입니다. 창을 닫습니다.");
        CloseHandle(hMutex);
        return 1;
    }

    if (!ConfigManager::GetInstance().LoadConfig("config.json")) {
        LOG_FATAL("System", "config 설정 파일 오류로 인해 LoginServer 종료합니다.");
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

        // 시그널 핸들러용 포인터 저장
        g_main_io_context_login = &io_context;

        ctx.worldConnection = std::make_shared<WorldConnection>(std::ref(io_context));
        short world_port = ConfigManager::GetInstance().GetLoginWorldConnPort();
        ctx.worldConnection->Connect("127.0.0.1", world_port);

        short login_port = ConfigManager::GetInstance().GetLoginServerPort();
        LoginServer server(io_context, login_port);
        LOG_INFO("LoginServer", "로그인 서버 가동 시작 (Port: " << login_port << ") Created by Jeong Shin Young");

        // ==========================================
        // DB 스레드: raw new → make_unique 사용
        // ==========================================
        int db_thread_count = ConfigManager::GetInstance().GetLoginDbThreadCount();
        LOG_INFO("System", "DB 연산 전용 백그라운드 스레드 가동 (" << db_thread_count << "개)...");

        for (int i = 0; i < db_thread_count; ++i) {
            ctx.db_threads_.emplace_back([i, &ctx]() {
                if (ConfigManager::GetInstance().UseDB()) {
                    // raw new → make_unique
                    t_dbManager = std::make_unique<DBManager>();
                    if (!t_dbManager->Connect()) {
                        LOG_ERROR("DB", "Thread " << i << " DB 연결 실패!");
                    }
                }

                ctx.db_io_context.run();

                // unique_ptr이므로 delete 불필요 (자동 해제)
                t_dbManager.reset();
                LOG_INFO("DB", "Thread " << i << " 정상 종료됨.");
            });
        }

        unsigned int max_thread_count = ConfigManager::GetInstance().GetLoginMaxThreadCount();
        if (max_thread_count == 0) max_thread_count = std::thread::hardware_concurrency();

        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < max_thread_count; ++i) {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }
        for (auto& t : threads) { if (t.joinable()) t.join(); }
    }
    catch (std::exception& e) {
        LOG_FATAL("Error", "서버 예외 발생: " << e.what());
    }

    g_main_io_context_login = nullptr;

    // Graceful Shutdown
    ctx.Shutdown();

    return 0;
}
