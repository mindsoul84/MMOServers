#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <atomic>

#include "protocol.pb.h"
#include "PacketDispatcher.h"

// 외부 라이브러리(absl, protobuf 등)에서 발생하는 초기화 누락 및 noexcept 관련 경고 무시
#pragma warning(disable: 26495) // 변수가 초기화되지 않았습니다.
#pragma warning(disable: 26439) // 이 종류의 함수는 throw하지 않아야 합니다.
#pragma warning(disable: 26451) // 산술 오버플로
#pragma warning(disable: 26812) // enum class 사용 권장

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

class Session;
class WorldConnection;

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

// ==========================================
// ★ WorldConnection 클래스 선언 (Login -> World)
// ==========================================
class WorldConnection : public std::enable_shared_from_this<WorldConnection> {
private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context& io_context_;
    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    WorldConnection(boost::asio::io_context& io_context);
    void Connect(const std::string& ip, short port);
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
};

// ==========================================
// ★ Session 클래스 선언 (Client -> Login)
// ==========================================
class Session : public std::enable_shared_from_this<Session> {
private:
    boost::asio::ip::tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;
    std::string logged_in_id_ = "";

public:
    Session(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void SetLoggedInId(const std::string& id) { logged_in_id_ = id; }
    const std::string& GetLoggedInId() const { return logged_in_id_; }
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void OnDisconnected();
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
};