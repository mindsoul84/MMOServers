#include "ClientSession.h"
#include "../Network/GameConnection.h"
#include "../../Common/Define/GameConstants.h"
#include "..\Common\MemoryPool.h"
#include "..\Common\Utils\NetworkErrorHandler.h"
#include "..\Common\Utils\Logger.h"
#include <iostream>

ClientSession::ClientSession(boost::asio::ip::tcp::socket socket) noexcept
    : socket_(std::move(socket))
    , strand_(static_cast<boost::asio::io_context&>(socket_.get_executor().context()))
{}

void ClientSession::start() { ReadHeader(); }

void ClientSession::SetAccountId(const std::string& id) { account_id_ = id; }
const std::string& ClientSession::GetAccountId() const { return account_id_; }

void ClientSession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) {
        LOG_WARN("Gateway", "Send 시도했으나 소켓이 이미 닫혀있음 (PktID: " << pktId << ")");
        return;
    }

    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    uint16_t totalSize = sizeof(PacketHeader) + payloadSize;

    if (totalSize > MAX_PACKET_SIZE) {
        LOG_ERROR("Gateway", "패킷 크기 초과! (PktID: " << pktId << ", Size: " << totalSize << " bytes) - 전송 취소");
        return;
    }

    SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();
    std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());

    PacketHeader header{ totalSize, pktId };
    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    msg.SerializeToArray(send_buf->buffer_.data() + sizeof(PacketHeader), payloadSize);

    auto self(shared_from_this());

    boost::asio::post(strand_, [this, self, send_buf, totalSize]() {
        if (send_queue_.size() > GameConstants::Network::SEND_QUEUE_MAX_SIZE) {
            LOG_WARN("Gateway", "ClientSession Send Queue 폭발! 전송 드랍.");
            return;
        }

        bool write_in_progress = !send_queue_.empty();
        send_queue_.emplace_back(send_buf, static_cast<size_t>(totalSize));

        if (!write_in_progress) {
            DoWrite();
        }
    });
}

void ClientSession::DoWrite() {
    auto self(shared_from_this());
    auto& front_msg = send_queue_.front();

    boost::asio::async_write(socket_,
        boost::asio::buffer(front_msg.first->buffer_.data(), front_msg.second),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                send_queue_.pop_front();
                if (!send_queue_.empty()) {
                    DoWrite();
                }
            }
            else {
                auto result = NetworkUtils::HandleError("ClientSession::DoWrite", ec);
                LOG_ERROR("Gateway", "Client로 패킷 전송 실패 (DoWrite)");
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
        LOG_INFO("Gateway", "유저 접속 종료 및 맵에서 삭제됨: " << account_id_);
        account_id_ = "";
    }
}

void ClientSession::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                // [패킷 파이프라인] 헤더 크기 검증
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) {
                    LOG_WARN("Gateway", "잘못된 패킷 헤더 크기: " << header_.size
                        << " (유저: " << account_id_ << ") - 연결 종료");
                    OnDisconnected();
                    return;
                }

                // [패킷 파이프라인] Rate Limiting 적용
                if (!rate_limiter_.AllowPacket()) {
                    rate_violation_count_++;
                    LOG_WARN("Gateway", "Rate limit 초과! (유저: " << account_id_
                        << ", 위반 횟수: " << rate_violation_count_ << "/" << MAX_VIOLATIONS << ")");

                    if (rate_violation_count_ >= MAX_VIOLATIONS) {
                        LOG_ERROR("Gateway", "Rate limit 연속 초과로 연결 강제 종료: " << account_id_);
                        OnDisconnected();
                        return;
                    }

                    // 패킷 드랍: 페이로드를 읽되 처리하지 않음
                    uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                    if (payload_size > 0) {
                        payload_buf_.resize(payload_size);
                        auto self2 = self;
                        boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
                            [this, self2](boost::system::error_code ec2, std::size_t) {
                                if (!ec2) ReadHeader(); // 드랍 후 다음 패킷 계속 수신
                                else OnDisconnected();
                            });
                    }
                    else {
                        ReadHeader();
                    }
                    return;
                }
                rate_violation_count_ = 0;  // 정상 패킷 시 위반 카운터 리셋

                // [패킷 파이프라인] 패킷 ID 유효성 사전 검증
                if (!GatewayContext::Get().clientDispatcher.HasHandler(header_.id)) {
                    LOG_WARN("Gateway", "미등록 패킷 ID: " << header_.id
                        << " (유저: " << account_id_ << ") - 패킷 드랍");
                    uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                    if (payload_size > 0) {
                        payload_buf_.resize(payload_size);
                        auto self2 = self;
                        boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
                            [this, self2](boost::system::error_code ec2, std::size_t) {
                                if (!ec2) ReadHeader();
                                else OnDisconnected();
                            });
                    }
                    else {
                        ReadHeader();
                    }
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
