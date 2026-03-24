#include "WorldConnection.h"
#include <iostream>

using boost::asio::ip::tcp;

WorldConnection::WorldConnection(boost::asio::io_context& io_context)
    : socket_(io_context), io_context_(io_context) {
}

void WorldConnection::Connect(const std::string& ip, short port) {
    tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(ip, std::to_string(port));
    boost::asio::connect(socket_, endpoints);

    try {
        unsigned short my_port = socket_.local_endpoint().port();
        std::cout << "[LoginServer] 🌐 WorldServer(S2S)에 성공적으로 연결되었습니다! (부여 포트: " << my_port << ")\n";
    }
    catch (...) {
        std::cout << "[LoginServer] 🌐 WorldServer(S2S)에 성공적으로 연결되었습니다!\n";
    }
    ReadHeader();
}

void WorldConnection::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    std::string payload;
    msg.SerializeToString(&payload);
    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
    header.id = pktId;

    auto send_buf = std::make_shared<std::vector<char>>(header.size);
    memcpy(send_buf->data(), &header, sizeof(PacketHeader));
    memcpy(send_buf->data() + sizeof(PacketHeader), payload.data(), payload.size());

    auto self(shared_from_this());
    boost::asio::async_write(socket_, boost::asio::buffer(*send_buf),
        [this, self, send_buf](boost::system::error_code ec, std::size_t) {
            if (ec) std::cerr << "[LoginServer] WorldServer로 패킷 전송 실패\n";
        });
}

void WorldConnection::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                if (payload_size == 0) {
                    auto session_ptr = self;
                    g_world_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                    ReadHeader();
                }
                else {
                    payload_buf_.resize(payload_size);
                    ReadPayload(payload_size);
                }
            }
            else std::cerr << "[LoginServer] 🚨 WorldServer와의 연결이 끊어졌습니다!\n";
        });
}

void WorldConnection::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                g_world_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
        });
}