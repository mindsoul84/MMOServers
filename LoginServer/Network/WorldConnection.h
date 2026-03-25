#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <deque>
#include <utility>
#include <string>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "../LoginServer.h"

// ==========================================
// ★ WorldConnection 클래스 선언 (Login -> World)
// ==========================================
class WorldConnection : public std::enable_shared_from_this<WorldConnection> {
private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context& io_context_;

    // ★ [버그 픽스] strand_ + send_queue_ 추가
    // 여러 Session에서 동시에 Send() 호출 시 concurrent async_write 방지
    boost::asio::io_context::strand strand_;
    std::deque<std::pair<std::shared_ptr<std::vector<char>>, size_t>> send_queue_;

    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    WorldConnection(boost::asio::io_context& io_context);
    void Connect(const std::string& ip, short port);
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);

    // ★ [버그 픽스] 큐에서 패킷을 꺼내 실제로 전송하는 내부 함수
    void DoWrite();
};
