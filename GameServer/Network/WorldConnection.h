#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include <google/protobuf/message.h>

struct PacketHeader; // 전방 선언

class WorldConnection : public std::enable_shared_from_this<WorldConnection> {
private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context& io_context_;
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
};