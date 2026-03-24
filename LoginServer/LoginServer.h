#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <atomic>

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
// ★ [리팩토링] LoginServer의 모든 상태를 관리하는 단일 Context
// GameServer와 동일한 패턴으로 통일
// ==========================================
struct LoginContext {
    // ---------------------------------------------------------
    // 1. DB 전용 io_context (블로킹 방지)
    // ---------------------------------------------------------
    boost::asio::io_context db_io_context;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> db_work_guard;

    // ---------------------------------------------------------
    // 2. 클라이언트 연결 관리
    // ---------------------------------------------------------
    std::atomic<int> connected_clients{ 0 };
    std::unordered_set<std::string> loggedInUsers;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessionMap;
    std::mutex loginMutex;

    // ---------------------------------------------------------
    // 3. 패킷 디스패처
    // ---------------------------------------------------------
    PacketDispatcher<Session> clientDispatcher;
    PacketDispatcher<WorldConnection> worldDispatcher;

    // ---------------------------------------------------------
    // 4. S2S 커넥션
    // ---------------------------------------------------------
    std::shared_ptr<WorldConnection> worldConnection;

    // ---------------------------------------------------------
    // 싱글톤 접근자
    // ---------------------------------------------------------
    static LoginContext& Get() {
        static LoginContext instance;
        return instance;
    }

private:
    LoginContext()
        : db_work_guard(boost::asio::make_work_guard(db_io_context)) {
    }

    // 복사/이동 금지
    LoginContext(const LoginContext&) = delete;
    LoginContext& operator=(const LoginContext&) = delete;
};

// ==========================================
// ★ [하위 호환성] 기존 전역 변수 참조를 위한 매크로
// 점진적 마이그레이션을 위해 유지 (향후 제거 권장)
// ==========================================
#define g_db_io_context       (LoginContext::Get().db_io_context)
#define g_connected_clients   (LoginContext::Get().connected_clients)
#define g_loggedInUsers       (LoginContext::Get().loggedInUsers)
#define g_sessionMap          (LoginContext::Get().sessionMap)
#define g_loginMutex          (LoginContext::Get().loginMutex)
#define g_client_dispatcher   (LoginContext::Get().clientDispatcher)
#define g_world_dispatcher    (LoginContext::Get().worldDispatcher)
#define g_worldConnection     (LoginContext::Get().worldConnection)