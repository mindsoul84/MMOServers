#pragma once

// ==========================================
// ★ 외부 라이브러리(Boost, Protobuf)의 정적 분석 경고 무시
// ==========================================
#pragma warning(disable: 26495) // 변수가 초기화되지 않았습니다.
#pragma warning(disable: 26439) // 이 종류의 함수는 throw하지 않아야 합니다.
#pragma warning(disable: 26451) // 산술 오버플로
#pragma warning(disable: 26812) // enum class 사용 권장
#pragma warning(disable: 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)

#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <atomic>

#include "protocol.pb.h"
#include "PacketDispatcher.h"

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

class Session;
class WorldConnection;

// =================================================================================================
// ★ DB 스레드 풀 및 큐 설정 : 메인 네트워크 스레드만 사용 시 LoginServer DB 블로킹 병목 현상 고려
// =================================================================================================
extern boost::asio::io_context g_db_io_context;

// ==========================================
// ★ 전역 변수 extern 선언
// ==========================================
extern std::atomic<int> g_connected_clients;
extern std::unordered_set<std::string> g_loggedInUsers;
extern std::unordered_map<std::string, std::shared_ptr<Session>> g_sessionMap;
extern std::mutex g_loginMutex;

extern PacketDispatcher<Session> g_client_dispatcher;
extern PacketDispatcher<WorldConnection> g_world_dispatcher;
extern std::shared_ptr<WorldConnection> g_worldConnection;