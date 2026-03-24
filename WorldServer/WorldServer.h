#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <mutex>

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

class ServerSession;

// ★ 다른 cpp 파일들에서도 이 전역 변수들을 사용할 수 있도록 extern 선언
extern PacketDispatcher<ServerSession> g_s2s_dispatcher;
extern std::vector<std::shared_ptr<ServerSession>> g_serverSessions;
extern std::mutex g_serverSessionMutex;

// Session 클래스 선언부
class ServerSession : public std::enable_shared_from_this<ServerSession> {
private:
    boost::asio::ip::tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    ServerSession(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
    void OnDisconnected();  // ★ [추가] 세션 정리 함수
};