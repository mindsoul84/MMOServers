#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <deque>
#include <utility>
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

extern PacketDispatcher<ServerSession> g_s2s_dispatcher;
extern std::vector<std::shared_ptr<ServerSession>> g_serverSessions;
extern std::mutex g_serverSessionMutex;

// ==========================================
// ServerSession: strand + send_queue 패턴 적용
//
// 변경 전: async_write를 strand 없이 직접 호출
//   -> 여러 스레드에서 동시에 Send() 호출 시 TCP 스트림 오염
// 변경 후: 모든 Send() 호출이 strand_ 내부에서 직렬화
//   -> GatewaySession, ClientSession과 동일한 안전성 확보
// ==========================================
class ServerSession : public std::enable_shared_from_this<ServerSession> {
private:
    boost::asio::ip::tcp::socket socket_;

    // strand + send_queue: concurrent async_write 방지
    boost::asio::io_context::strand strand_;
    std::deque<std::pair<std::shared_ptr<struct SendBuffer>, size_t>> send_queue_;

    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    ServerSession(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
    void OnDisconnected();
    void DoWrite();  // 큐에서 패킷을 꺼내 실제로 전송하는 내부 함수
};
