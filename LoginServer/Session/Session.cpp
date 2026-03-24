#include "Session.h"
#include "../../Common/MemoryPool.h"
#include <iostream>

using boost::asio::ip::tcp;

Session::Session(tcp::socket socket) noexcept : socket_(std::move(socket)) {}

void Session::start() { ReadHeader(); }

void Session::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) return;

#ifdef  DEF_STRESS_TEST_TOOL
    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    uint16_t totalSize = sizeof(PacketHeader) + payloadSize;

    // =========================================================
    // 버퍼 크기 초과 시 서버 죽지 않도록 에러 로그만 띄우고 취소
    // =========================================================
    if (totalSize > MAX_PACKET_SIZE) {
        std::cerr << "🚨 [Error] 패킷 크기 초과! (PktID: " << pktId
            << ", Size: " << totalSize << " bytes) - 전송 취소\n";
        return;
    }
#endif//DEF_STRESS_TEST_TOOL

    std::string payload;
    msg.SerializeToString(&payload);
    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
    header.id = pktId;

    SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();
    std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());

    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    memcpy(send_buf->buffer_.data() + sizeof(PacketHeader), payload.data(), payload.size());

    auto self(shared_from_this());
    boost::asio::async_write(socket_, boost::asio::buffer(send_buf->buffer_.data(), header.size),
        [this, self, send_buf](boost::system::error_code ec, std::size_t) {
            if (ec) {} // 에러 로깅
        });
}

void Session::OnDisconnected() {
    int current_count = --g_connected_clients;
    std::cout << "[LoginServer] 유저 접속 종료 (접속자: " << current_count << "명)\n";
    if (!logged_in_id_.empty()) {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        g_loggedInUsers.erase(logged_in_id_);
        g_sessionMap.erase(logged_in_id_);
    }
}

void Session::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) return;
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                if (payload_size == 0) {
                    auto session_ptr = self;
                    g_client_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                    ReadHeader();
                }
                else {
                    payload_buf_.resize(payload_size);
                    ReadPayload(payload_size);
                }
            }
            else OnDisconnected();
        });
}

void Session::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                g_client_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else OnDisconnected();
        });
}