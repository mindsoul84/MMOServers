#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <vector>
#include <google/protobuf/message.h>
#include "..\GatewayServer.h"

class ClientSession : public std::enable_shared_from_this<ClientSession> {
private:
    boost::asio::ip::tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;
    std::string account_id_ = "";

public:
    ClientSession(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void SetAccountId(const std::string& id);
    const std::string& GetAccountId() const;
    void Send(uint16_t pktId, const google::protobuf::Message& msg);
    void OnDisconnected();

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
};