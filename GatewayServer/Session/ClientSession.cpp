#include "ClientSession.h"
#include "../Network/GameConnection.h"
#include "..\Common\MemoryPool.h"
#include <iostream>

ClientSession::ClientSession(boost::asio::ip::tcp::socket socket) noexcept : socket_(std::move(socket)) {}

void ClientSession::start() { ReadHeader(); }

void ClientSession::SetAccountId(const std::string& id) { account_id_ = id; }
const std::string& ClientSession::GetAccountId() const { return account_id_; }

void ClientSession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
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

    // 메모리 풀의 64KB 고정 버퍼를 쓰지 않고, 딱 필요한 만큼(totalSize)만 할당
    auto send_buf = std::make_shared<std::vector<char>>(totalSize);

    PacketHeader header{ totalSize, pktId };
    memcpy(send_buf->data(), &header, sizeof(PacketHeader));
    msg.SerializeToArray(send_buf->data() + sizeof(PacketHeader), payloadSize);

    auto self(shared_from_this());
    boost::asio::async_write(socket_, boost::asio::buffer(send_buf->data(), totalSize),
        [this, self, send_buf](boost::system::error_code ec, std::size_t) {});

#else //DEF_STRESS_TEST_TOOL

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

#endif//DEF_STRESS_TEST_TOOL    
}

void ClientSession::OnDisconnected() {
    if (!account_id_.empty()) {
        auto& ctx = GatewayContext::Get();

        // ★ [수정] g_gameConnection → ctx.gameConnection
        if (ctx.gameConnection) {
            Protocol::GatewayGameLeaveReq leave_req;
            leave_req.set_account_id(account_id_);
            ctx.gameConnection->Send(Protocol::PKT_GATEWAY_GAME_LEAVE_REQ, leave_req);
        }

        // ★ [수정] g_clientMutex, g_clientMap → ctx.clientMutex, ctx.clientMap
        std::lock_guard<std::mutex> lock(ctx.clientMutex);
        ctx.clientMap.erase(account_id_);
        std::cout << "[Gateway] 유저 접속 종료 및 맵에서 삭제됨: " << account_id_ << "\n";
        account_id_ = "";
    }
}

void ClientSession::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) return;
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                if (payload_size == 0) {
                    auto session_ptr = self;
                    // ★ [수정] g_gateway_dispatcher → GatewayContext::Get().clientDispatcher
                    GatewayContext::Get().clientDispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
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
                // ★ [수정] g_gateway_dispatcher → GatewayContext::Get().clientDispatcher
                GatewayContext::Get().clientDispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else OnDisconnected();
        });
}
