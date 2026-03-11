#include "ClientSession.h"
#include "../Network/GameConnection.h" // S2S 접속 객체 참조용
#include "..\Common\MemoryPool.h"
#include <iostream>

ClientSession::ClientSession(boost::asio::ip::tcp::socket socket) noexcept : socket_(std::move(socket)) {}

void ClientSession::start() { ReadHeader(); }

void ClientSession::SetAccountId(const std::string& id) { account_id_ = id; }
const std::string& ClientSession::GetAccountId() const { return account_id_; }

void ClientSession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) return;

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
        [this, self, send_buf](boost::system::error_code ec, std::size_t) {});
}

void ClientSession::OnDisconnected() {
    if (!account_id_.empty()) {
        if (g_gameConnection) {
            Protocol::GatewayGameLeaveReq leave_req;
            leave_req.set_account_id(account_id_);
            g_gameConnection->Send(Protocol::PKT_GATEWAY_GAME_LEAVE_REQ, leave_req);
        }

        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_clientMap.erase(account_id_);
        std::cout << "[Gateway] 유저 접속 종료 및 맵에서 삭제됨: " << account_id_ << "\n";
        account_id_ = "";
    }
}

void ClientSession::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > 4096) return;
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                if (payload_size == 0) {
                    auto session_ptr = self;
                    g_gateway_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
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

void ClientSession::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                g_gateway_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else OnDisconnected();
        });
}