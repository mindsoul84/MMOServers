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

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

class Session;
class WorldConnection;

// ==========================================
// [수정] LoginContext 싱글톤 → 의존성 주입(DI) 지원
//
// 변경 전: private 생성자 + static Get() → 테스트 불가
// 변경 후: public 생성자 + SetTestInstance() → 테스트에서 목(mock) 주입 가능
//
// [수정] thread::detach 남용 → db_threads_ + Shutdown()으로 안전한 종료
//   - StartDbThreads()로 등록된 DB 스레드는 Shutdown()에서 join됨
//   - graceful shutdown: db_io_context.stop() → join
// ==========================================
struct LoginContext {
    boost::asio::io_context db_io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> db_work_guard;

    std::atomic<int> connected_clients{ 0 };
    std::unordered_set<std::string> loggedInUsers;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessionMap;
    std::mutex loginMutex;

    PacketDispatcher<Session> clientDispatcher;
    PacketDispatcher<WorldConnection> worldDispatcher;

    std::shared_ptr<WorldConnection> worldConnection;

    // ★ [추가 - 수정] DB 전담 스레드를 detach 대신 여기에 보관 → Shutdown()에서 join
    std::vector<std::thread> db_threads_;

    // ★ [추가 - 수정] 정상 종료 신호 플래그
    std::atomic<bool> is_running_{ true };

    // [수정] 테스트 인스턴스 오버라이드 포인터
    // [수정] inline 정의 (C++17) → 별도 .cpp 없이 링크 완결
    inline static LoginContext* s_test_instance_ = nullptr;

    // [수정] 테스트 인스턴스가 주입된 경우 반환, 없으면 정적 싱글톤 반환
    static LoginContext& Get() {
        if (s_test_instance_) return *s_test_instance_;
        static LoginContext instance;
        return instance;
    }

    // ★ [추가 - 수정] 테스트 전용 주입 메서드
    static void SetTestInstance(LoginContext* instance) noexcept {
        s_test_instance_ = instance;
    }

    // ★ [추가 - 수정] Graceful Shutdown:
    //   1) is_running_ = false 로 루프 종료 신호
    //   2) db_work_guard 해제 + db_io_context 정지 → DB 스레드 종료
    //   3) 모든 db_threads_ join
    void Shutdown() {
        is_running_.store(false);
        db_work_guard.reset();      // work_guard 해제 → run()이 반환될 수 있게
        db_io_context.stop();
        for (auto& t : db_threads_) {
            if (t.joinable()) t.join();
        }
        db_threads_.clear();
        std::cout << "[LoginContext] Shutdown 완료: 모든 DB 스레드 종료됨.\n";
    }

    // [수정] public 생성자 → 테스트 코드에서 직접 인스턴스 생성 가능
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
