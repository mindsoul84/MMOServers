
#include "LoginServer.h"
#include "Session/Session.h"
#include "Network/WorldConnection.h"
#include "Handlers/ClientLogin/ClientHandlers.h"
#include "Handlers/LoginWorld/WorldHandlers.h"

#include "..\Common\ConfigManager.h"
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

    // 서버 역할에 맞는 메모리 풀 초기화 (LoginServer는 경량 서버)
    SendBufferPool::GetInstance().Initialize(PoolConfig::LIGHT_SERVER);

    auto& ctx = LoginContext::Get();

    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_LOGIN_LOGIN_REQ,        Handle_LoginReq);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_SERVER_HEARTBEAT,       Handle_Heartbeat);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_LOGIN_WORLD_SELECT_REQ, Handle_WorldSelectReq);
    ctx.worldDispatcher.RegisterHandler(Protocol::PKT_WORLD_LOGIN_SELECT_RES,         Handle_S2SWorldSelectRes);

    ctx.worldDispatcher.RegisterHandler(Protocol::PKT_WORLD_GAME_TOKEN_NOTIFY,
        [](std::shared_ptr<WorldConnection>&, char*, uint16_t) {
            // LoginServer에서는 토큰 통지를 처리하지 않음 (GameServer 전용)
        });

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
        //   thread_local DBManager → DBConnectionPool 전환
        //
        // 변경 전: 각 DB 스레드가 thread_local로 고유 ODBC 연결 생성
        //   -> 스레드 수와 연결 수가 1:1로 고정
        //   -> 스레드 종료 시 수동 정리 필요 (t_dbManager.reset())
        //
        // 변경 후: DB 스레드 시작 전에 연결 풀을 미리 초기화
        //   -> 스레드는 io_context.run()만 실행 (연결 관리 불필요)
        //   -> 핸들러에서 pool.Acquire()로 연결 획득, 스코프 종료 시 자동 반납
        //   -> 풀 크기를 스레드 수의 2배로 설정하여 여유분 확보
        // ==========================================
        int db_thread_count = ConfigManager::GetInstance().GetLoginDbThreadCount();

        if (ConfigManager::GetInstance().UseDB()) {
            // 풀 크기 = 스레드 수 * 2 (동시 요청 급증 시 여유분)
            size_t pool_size = static_cast<size_t>(db_thread_count) * 2;
            if (!ctx.db_pool_.Initialize(pool_size)) {
                LOG_WARN("System", "DB 연결 풀 초기화 실패. DB 기능이 제한됩니다.");
            }
        }
        else {
            LOG_WARN("System", "config.json 설정에 따라 DB 연동을 건너뜁니다.");
        }

        LOG_INFO("System", "DB 연산 전용 백그라운드 스레드 가동 (" << db_thread_count << "개)...");

        for (int i = 0; i < db_thread_count; ++i) {
            ctx.db_threads_.emplace_back([i, &ctx]() {
                //   thread_local DBManager 생성 제거
                // 연결 풀에서 필요할 때 Acquire하므로 스레드별 연결이 불필요
                ctx.db_io_context.run();
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
