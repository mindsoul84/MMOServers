#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "PacketDispatcher.h"
#include "../Common/DB/DBConnectionPool.h"
#include "../Common/Utils/Lock.h"

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

class Session;
class WorldConnection;

// ==========================================
//   thread_local DBManager -> DBConnectionPool로 교체
//   변경 전: 각 DB 스레드가 thread_local로 고유 ODBC 연결을 소유
//     -> 스레드 수 = 연결 수 (1:1 고정)
//     -> 연결 수 확장 시 스레드도 같이 늘려야 함
//
//   변경 후: DBConnectionPool이 연결을 중앙 관리
//     -> 스레드 수와 연결 수를 독립적으로 설정 가능
//     -> ScopedConnection RAII로 자동 반납 (누수 방지)
//     -> DB 스레드는 io_context.run()만 수행 (연결 관리 불필요)
//
//   thread::detach 남용 -> db_threads_ + Shutdown()으로 안전한 종료
// ==========================================
struct LoginContext {
    boost::asio::io_context db_io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> db_work_guard;

    std::atomic<int> connected_clients{ 0 };
    std::unordered_set<std::string> loggedInUsers;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessionMap;
    UTILITY::Lock loginMutex;

    PacketDispatcher<Session> clientDispatcher;
    PacketDispatcher<WorldConnection> worldDispatcher;

    std::shared_ptr<WorldConnection> worldConnection;

    //   thread_local DBManager -> 연결 풀로 교체
    DBConnectionPool db_pool_;

    // DB 전담 스레드를 detach 대신 여기에 보관 -> Shutdown()에서 join
    std::vector<std::thread> db_threads_;

    // 정상 종료 신호 플래그
    std::atomic<bool> is_running_{ true };

    //   테스트 인스턴스 오버라이드 포인터
    inline static LoginContext* s_test_instance_ = nullptr;

    static LoginContext& Get() {
        if (s_test_instance_) return *s_test_instance_;
        static LoginContext instance;
        return instance;
    }

    static void SetTestInstance(LoginContext* instance) noexcept {
        s_test_instance_ = instance;
    }

    void Shutdown() {
        is_running_.store(false);
        db_work_guard.reset();
        db_io_context.stop();
        for (auto& t : db_threads_) {
            if (t.joinable()) t.join();
        }
        db_threads_.clear();
        std::cout << "[LoginContext] Shutdown 완료: 모든 DB 스레드 종료됨.\n";
    }

    LoginContext()
        : db_work_guard(boost::asio::make_work_guard(db_io_context)) {
    }

    LoginContext(const LoginContext&) = delete;
    LoginContext& operator=(const LoginContext&) = delete;
};

// 하위 호환성 매크로 (기존 코드가 g_xxx 이름으로 접근하는 곳에서 사용)
#define g_db_io_context       (LoginContext::Get().db_io_context)
#define g_connected_clients   (LoginContext::Get().connected_clients)
#define g_loggedInUsers       (LoginContext::Get().loggedInUsers)
#define g_sessionMap          (LoginContext::Get().sessionMap)
#define g_loginMutex          (LoginContext::Get().loginMutex)
#define g_client_dispatcher   (LoginContext::Get().clientDispatcher)
#define g_world_dispatcher    (LoginContext::Get().worldDispatcher)
#define g_worldConnection     (LoginContext::Get().worldConnection)
