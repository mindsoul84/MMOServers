#include "ClientSession.h"
#include "../Network/GameConnection.h"
#include "../../Common/Define/GameConstants.h"
#include "../../Common/Define/SecurityConstants.h"
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

// ==========================================
//   암호화 활성화
//
// GatewayConnectReq/Res 핸드셰이크가 성공한 뒤 호출됩니다.
// 사전 공유 패스프레이즈에서 AES-128 키를 도출하고 암호화를 활성화합니다.
//
// 이 메서드 호출 이후의 모든 Send/ReadPayload는 암호화된 페이로드를 처리합니다.
// GatewayConnectRes는 핸드셰이크 패킷이므로 평문으로 전송된 뒤 활성화됩니다.
// ==========================================
void ClientSession::EnableEncryption() {
    if (crypto_.InitializeWithPassphrase(SecurityConstants::Crypto::SHARED_PASSPHRASE)) {
        crypto_enabled_ = true;
        LOG_INFO("Gateway", "패킷 암호화 활성화 (유저: " << account_id_ << ")");
    }
    else {
        LOG_ERROR("Gateway", "패킷 암호화 초기화 실패 (유저: " << account_id_ << ")");
    }
}

bool ClientSession::OnParseViolation() {
    if (parse_tracker_.OnParseFailure()) {
        LOG_ERROR("Gateway", "ParseFromArray 연속 실패 임계값 초과 (유저: "
            << account_id_ << ", 위반 횟수: "
            << parse_tracker_.GetViolationCount() << ") - 연결 강제 종료");
        return true;
    }
    LOG_WARN("Gateway", "ParseFromArray 실패 (유저: " << account_id_
        << ", 누적 위반: " << parse_tracker_.GetViolationCount()
        << "/" << SecurityConstants::Packet::MAX_PARSE_VIOLATIONS << ")");
    return false;
}

void ClientSession::OnParseSuccess() {
    parse_tracker_.OnParseSuccess();
}

// ==========================================
//   Send() - 암호화 통합
//
// 변경 전: Protobuf 직렬화 → 평문 페이로드를 그대로 전송
// 변경 후: Protobuf 직렬화 → 암호화(활성 시) → 암호화된 페이로드 전송
//   PacketHeader.size에는 암호화 오버헤드(SeqNum+IV+padding)가 포함됨
// ==========================================
void ClientSession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) {
        LOG_WARN("Gateway", "Send 시도했으나 소켓이 이미 닫혀있음 (PktID: " << pktId << ")");
        return;
    }

    // 1. Protobuf 직렬화
    std::string serialized;
    msg.SerializeToString(&serialized);

    // 2. 암호화 (활성화된 경우)
    std::vector<char> final_payload;
    if (crypto_enabled_ && crypto_.IsInitialized()) {
        auto result = crypto_.Encrypt(serialized.data(), static_cast<uint16_t>(serialized.size()));
        if (result.success) {
            final_payload = std::move(result.data);
        }
        else {
            LOG_ERROR("Gateway", "패킷 암호화 실패 (PktID: " << pktId << ") - " << result.error_message);
            return;
        }
    }
    else {
        final_payload.assign(serialized.begin(), serialized.end());
    }

    // 3. 패킷 헤더 구성 (암호화 오버헤드 포함)
    uint16_t totalSize = static_cast<uint16_t>(sizeof(PacketHeader) + final_payload.size());

    if (totalSize > MAX_PACKET_SIZE) {
        LOG_ERROR("Gateway", "패킷 크기 초과! (PktID: " << pktId << ", Size: " << totalSize << " bytes) - 전송 취소");
        return;
    }

    SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();
    std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());

    PacketHeader header{ totalSize, pktId };
    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    memcpy(send_buf->buffer_.data() + sizeof(PacketHeader), final_payload.data(), final_payload.size());

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

        UTILITY::LockGuard lock(ctx.clientMutex);
        ctx.clientMap.erase(account_id_);
        LOG_INFO("Gateway", "유저 접속 종료 및 맵에서 삭제됨: " << account_id_);
        account_id_ = "";
    }
}

// ==========================================
// [참고] boost::asio::async_read의 완전 수신 보장 (composed operation)
//
// boost::asio::async_read는 요청한 바이트 수를 모두 수신할 때까지
// 내부적으로 async_read_some을 반복 호출하는 composed operation입니다.
// TCP 스트림에서 PacketHeader(4바이트)가 여러 TCP 세그먼트로 분할 수신되더라도,
// 콜백은 반드시 4바이트를 모두 받은 후에만 호출됩니다.
//
// 이와 달리 socket.async_read_some()은 사용 가능한 만큼만 읽으므로
// 부분 수신이 발생할 수 있습니다. 이 프로젝트에서는 의도적으로
// async_read(전체 읽기 보장)만 사용합니다.
// (ref: boost.org/doc/libs/release/doc/html/boost_asio/reference/async_read.html)
// ==========================================
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
                        << ", 위반 횟수: " << rate_violation_count_
                        << "/" << SecurityConstants::Packet::MAX_RATE_VIOLATIONS << ")");

                    if (rate_violation_count_ >= SecurityConstants::Packet::MAX_RATE_VIOLATIONS) {
                        LOG_ERROR("Gateway", "Rate limit 연속 초과로 연결 강제 종료: " << account_id_);
                        OnDisconnected();
                        return;
                    }

                    // 패킷 드랍: 페이로드를 읽되 처리하지 않음
                    uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                    if (payload_size > 0) {
                        if (!assembler_.ValidatePayloadSize(payload_size)) {
                            OnDisconnected();
                            return;
                        }
                        auto self2 = self;
                        boost::asio::async_read(socket_,
                            boost::asio::buffer(assembler_.GetBuffer(), payload_size),
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
                rate_violation_count_ = 0;

                // [패킷 파이프라인] 패킷 ID 유효성 사전 검증
                if (!GatewayContext::Get().clientDispatcher.HasHandler(header_.id)) {
                    LOG_WARN("Gateway", "미등록 패킷 ID: " << header_.id
                        << " (유저: " << account_id_ << ") - 패킷 드랍");
                    uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                    if (payload_size > 0) {
                        if (!assembler_.ValidatePayloadSize(payload_size)) {
                            OnDisconnected();
                            return;
                        }
                        auto self2 = self;
                        boost::asio::async_read(socket_,
                            boost::asio::buffer(assembler_.GetBuffer(), payload_size),
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
                    if (!assembler_.ValidatePayloadSize(payload_size)) {
                        LOG_WARN("Gateway", "페이로드 크기가 고정 버퍼 범위 초과: "
                            << payload_size << " (유저: " << account_id_ << ")");
                        OnDisconnected();
                        return;
                    }
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

// ==========================================
//   ReadPayload — 복호화 통합
//
// 변경 전: 수신된 바이트를 그대로 Dispatch에 전달
// 변경 후: 암호화 활성 시 수신된 바이트를 복호화한 뒤 Dispatch에 전달
//   복호화된 데이터는 원본 Protobuf 페이로드이므로
//   핸들러의 ParseFromArray가 정상 동작함
// ==========================================
void ClientSession::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_,
        boost::asio::buffer(assembler_.GetBuffer(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                char* dispatch_data = assembler_.GetPayloadData();
                uint16_t dispatch_size = payload_size;

                //   암호화 활성 시 복호화 수행
                std::vector<char> decrypted_buf;
                if (crypto_enabled_ && crypto_.IsInitialized()) {
                    auto result = crypto_.Decrypt(dispatch_data, payload_size);
                    if (result.success) {
                        decrypted_buf = std::move(result.data);
                        dispatch_data = decrypted_buf.data();
                        dispatch_size = static_cast<uint16_t>(decrypted_buf.size());
                    }
                    else {
                        LOG_ERROR("Gateway", "패킷 복호화 실패 (유저: " << account_id_
                            << ") - " << result.error_message);
                        OnDisconnected();
                        return;
                    }
                }

                auto session_ptr = self;
                GatewayContext::Get().clientDispatcher.Dispatch(
                    session_ptr, header_.id, dispatch_data, dispatch_size);
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
