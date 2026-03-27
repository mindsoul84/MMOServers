#include "ClientSession.h"
#include "../Network/GameConnection.h"
#include "../../Common/Define/GameConstants.h"
#include "..\Common\MemoryPool.h"
#include "..\Common\Utils\NetworkErrorHandler.h"
#include <iostream>

// ==========================================
// [BUG FIX] strand_ 초기화 추가
//
// 변경 전: socket_(std::move(socket)) 만 초기화
// 변경 후: strand_(socket_.get_executor()) 추가 -> Send() 직렬화 보장
// ==========================================
ClientSession::ClientSession(boost::asio::ip::tcp::socket socket) noexcept
    : socket_(std::move(socket))
    , strand_(static_cast<boost::asio::io_context&>(socket_.get_executor().context()))
{}

void ClientSession::start() { ReadHeader(); }

void ClientSession::SetAccountId(const std::string& id) { account_id_ = id; }
const std::string& ClientSession::GetAccountId() const { return account_id_; }

// ==========================================
// ★ [수정] Send() - 메모리 풀 활용으로 통일
//
// 변경 전: make_shared<SendBuffer>(totalSize) → 매번 힙 할당
//   → 대규모 동접 시 new/delete가 초당 수만 회 발생하여 힙 할당자 병목 유발
//
// 변경 후: SendBufferPool에서 대여 → 전송 완료 시 자동 반납 (SendBufferDeleter)
//   → Lock-Free 풀에서 O(1)로 버퍼 획득, 힙 할당 비용 사실상 0
// ==========================================
void ClientSession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) {
        std::cerr << "[Gateway] ⚠️ Send 시도했으나 소켓이 이미 닫혀있음 (PktID: " << pktId << ")\n";
        return;
    }

    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    uint16_t totalSize = sizeof(PacketHeader) + payloadSize;

    if (totalSize > MAX_PACKET_SIZE) {
        std::cerr << "🚨 [Error] 패킷 크기 초과! (PktID: " << pktId
            << ", Size: " << totalSize << " bytes) - 전송 취소\n";
        return;
    }

    // ★ [수정] 메모리 풀에서 버퍼 대여 (전송 완료 시 SendBufferDeleter가 자동 반납)
    SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();
    std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());

    PacketHeader header{ totalSize, pktId };
    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    msg.SerializeToArray(send_buf->buffer_.data() + sizeof(PacketHeader), payloadSize);

    auto self(shared_from_this());

    // [BUG FIX] strand_ 내부에서 큐에 쌓고 DoWrite()로 순차 전송
    boost::asio::post(strand_, [this, self, send_buf, totalSize]() {
        if (send_queue_.size() > GameConstants::Network::SEND_QUEUE_MAX_SIZE) {
            std::cerr << "[Warning] ClientSession Send Queue 폭발! 전송 드랍.\n";
            return;
        }

        bool write_in_progress = !send_queue_.empty();
        send_queue_.emplace_back(send_buf, static_cast<size_t>(totalSize));

        if (!write_in_progress) {
            DoWrite();
        }
    });
}

// ==========================================
// [BUG FIX] 실제 비동기 전송을 수행하는 함수
//
// strand_ 내부에서만 호출되며, 큐의 맨 앞 패킷부터 순차적으로 전송합니다.
// 전송 완료 콜백도 bind_executor(strand_)로 보호하여 스레드 안전성을 확보합니다.
// ==========================================
void ClientSession::DoWrite() {
    auto self(shared_from_this());
    auto& front_msg = send_queue_.front();

    boost::asio::async_write(socket_,
        boost::asio::buffer(front_msg.first->buffer_.data(), front_msg.second),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                send_queue_.pop_front();

                // 큐에 대기 중인 다음 패킷이 있다면 이어서 전송 (꼬리물기)
                if (!send_queue_.empty()) {
                    DoWrite();
                }
            }
            else {
                auto result = NetworkUtils::HandleError("ClientSession::DoWrite", ec);
                std::cerr << "[Gateway] Client로 패킷 전송 실패 (DoWrite)\n";
                send_queue_.clear();
                if (result.should_disconnect) {
                    OnDisconnected();
                }
            }
        }));
}

void ClientSession::OnDisconnected() {
    if (!account_id_.empty()) {
        auto& ctx = GatewayContext::Get();

        if (ctx.gameConnection) {
            Protocol::GatewayGameLeaveReq leave_req;
            leave_req.set_account_id(account_id_);
            ctx.gameConnection->Send(Protocol::PKT_GATEWAY_GAME_LEAVE_REQ, leave_req);
        }

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
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) {
                    std::cerr << "[Gateway] ⚠️ 잘못된 패킷 헤더 크기: " << header_.size << "\n";
                    return;
                }
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                if (payload_size == 0) {
                    auto session_ptr = self;
                    GatewayContext::Get().clientDispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                    ReadHeader();
                }
                else {
                    payload_buf_.resize(payload_size);
                    ReadPayload(payload_size);
                }
            }
            else {
                auto result = NetworkUtils::HandleError("ClientSession::ReadHeader", ec);
                if (result.should_disconnect || 
                    NetworkUtils::ClassifyError(ec) != NetworkUtils::ErrorSeverity::IGNORED_ERROR) {
                    OnDisconnected();
                }
            }
        });
}

void ClientSession::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                GatewayContext::Get().clientDispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else {
                auto result = NetworkUtils::HandleError("ClientSession::ReadPayload", ec);
                if (result.should_disconnect || 
                    NetworkUtils::ClassifyError(ec) != NetworkUtils::ErrorSeverity::IGNORED_ERROR) {
                    OnDisconnected();
                }
            }
        });
}
