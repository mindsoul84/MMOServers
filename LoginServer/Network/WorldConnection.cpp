#include "WorldConnection.h"
#include <iostream>

using boost::asio::ip::tcp;

WorldConnection::WorldConnection(boost::asio::io_context& io_context)
    : socket_(io_context)
    , io_context_(io_context)
    , strand_(io_context)   // ★ [버그 픽스] strand_ 초기화
{}

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

// ==========================================
// ★ [버그 픽스] Send() - strand + send_queue 패턴 적용
//
// 변경 전: async_write 직접 호출 → 동시 Send() 시 소켓 오염 가능
// 변경 후: strand_에 post → DoWrite()로 순차 전송
// ==========================================
void WorldConnection::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    std::string payload;
    msg.SerializeToString(&payload);
    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
    header.id = pktId;

    auto send_buf = std::make_shared<std::vector<char>>(header.size);
    memcpy(send_buf->data(), &header, sizeof(PacketHeader));
    memcpy(send_buf->data() + sizeof(PacketHeader), payload.data(), payload.size());

    size_t write_size = header.size;
    auto self(shared_from_this());

    boost::asio::post(strand_, [this, self, send_buf, write_size]() {
        bool write_in_progress = !send_queue_.empty();
        send_queue_.emplace_back(send_buf, write_size);
        if (!write_in_progress) DoWrite();
    });
}

void WorldConnection::DoWrite() {
    auto self(shared_from_this());
    auto& front = send_queue_.front();

    boost::asio::async_write(socket_,
        boost::asio::buffer(front.first->data(), front.second),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                send_queue_.pop_front();
                if (!send_queue_.empty()) DoWrite();
            }
            else {
                std::cerr << "[LoginServer] WorldServer로 패킷 전송 실패 (DoWrite): " << ec.message() << "\n";
                send_queue_.clear();
            }
        }));
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
            else {
                std::cerr << "[LoginServer] 🚨 WorldServer와의 연결이 끊어졌습니다!\n";
            }
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
            else {
                std::cerr << "[LoginServer] 🚨 WorldServer와의 연결이 끊어졌습니다!\n";
            }
        });
}
