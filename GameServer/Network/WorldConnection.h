#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <deque>
#include <utility>
#include <string>
#include <google/protobuf/message.h>

struct PacketHeader; // 전방 선언

class WorldConnection : public std::enable_shared_from_this<WorldConnection> {
private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context& io_context_;

    // strand_ + send_queue_ 추가
    // 여러 경로에서 동시에 Send() 호출 시 concurrent async_write 방지
    boost::asio::io_context::strand strand_;
    std::deque<std::pair<std::shared_ptr<std::vector<char>>, size_t>> send_queue_;

    uint16_t current_header_id_;
    uint16_t current_payload_size_;
    std::vector<char> payload_buf_;

public:
    WorldConnection(boost::asio::io_context& io_context);
    void Connect(const std::string& ip, short port);
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void ReadHeader();
    void ReadPayload();

    // 큐에서 패킷을 꺼내 실제로 전송하는 내부 함수
    void DoWrite();
};
