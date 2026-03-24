#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <vector>
#include <google/protobuf/message.h>
#include "../GatewayServer.h"

class GameConnection : public std::enable_shared_from_this<GameConnection> {
private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context& io_context_;
    boost::asio::steady_timer retry_timer_;
    PacketHeader header_;
    std::vector<char> payload_buf_;
    std::string target_ip_;
    short target_port_;

public:
    GameConnection(boost::asio::io_context& io_context);
    void Connect(const std::string& ip, short port);
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void DoConnect();
    void ScheduleRetry();
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
};