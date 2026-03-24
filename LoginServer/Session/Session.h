#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "../LoginServer.h"

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