#include "GatewaySession.h"
#include "..\GameServer\GameServer.h"
#include "../../Common/MemoryPool.h"
#include <iostream>

using boost::asio::ip::tcp;

GatewaySession::GatewaySession(tcp::socket socket) noexcept : socket_(std::move(socket)) {}

void GatewaySession::start() {
    ReadHeader();
}

void GatewaySession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) return;

    std::string payload;
    msg.SerializeToString(&payload);
    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
    header.id = pktId;

    // MemoryPool을 활용한 락프리 버퍼 할당
    SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();
    std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());

    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    memcpy(send_buf->buffer_.data() + sizeof(PacketHeader), payload.data(), payload.size());

    auto self(shared_from_this());
    boost::asio::async_write(socket_, boost::asio::buffer(send_buf->buffer_.data(), header.size),
        [this, self, send_buf](boost::system::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "[GameServer] Gateway로 S2S 패킷 전송 실패\n";
            }
        });
}

void GatewaySession::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > 4096) return;
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));

                if (payload_size == 0) {
                    auto session_ptr = self;
                    // ★ [수정] GameContext의 디스패처 사용
                    GameContext::Get().gatewayDispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                    ReadHeader();
                }
                else {
                    payload_buf_.resize(payload_size);
                    ReadPayload(payload_size);
                }
            }
            else {
                std::cout << "[GameServer] GatewayServer와의 S2S 연결 해제됨.\n";
                // ★ [추가] 연결 끊김 시 GameContext의 리스트에서 안전하게 제거
                auto& ctx = GameContext::Get();
                std::lock_guard<std::mutex> lock(ctx.gatewaySessionMutex);
                auto it = std::find(ctx.gatewaySessions.begin(), ctx.gatewaySessions.end(), self);
                if (it != ctx.gatewaySessions.end()) {
                    ctx.gatewaySessions.erase(it);
                }
            }
        });
}

void GatewaySession::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                // ★ [수정] GameContext의 디스패처 사용
                GameContext::Get().gatewayDispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
        });
}