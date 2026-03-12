#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <google/protobuf/message.h>
#include "../GameServer.h"

class GatewaySession : public std::enable_shared_from_this<GatewaySession> {
private:
    boost::asio::ip::tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    GatewaySession(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
};