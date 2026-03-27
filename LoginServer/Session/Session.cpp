#include "Session.h"
#include "../../Common/MemoryPool.h"
#include <iostream>

using boost::asio::ip::tcp;

// ★ [수정 3] heartbeat_timer_를 소켓의 executor에서 생성 (별도 io_context 불필요)
Session::Session(tcp::socket socket) noexcept
    : socket_(std::move(socket))
    , heartbeat_timer_(socket_.get_executor())
    , last_heartbeat_(std::chrono::steady_clock::now())
{}

void Session::start() {
    ReadHeader();
    // ★ [수정 3] 연결 즉시 하트비트 감시 타이머 시작
    StartHeartbeatCheck();
}

// ==========================================
// ★ [수정 3] Heartbeat 타임아웃 체크 구현
//
// HEARTBEAT_CHECK_INTERVAL_SECONDS마다 타이머가 깨어나서
// 마지막 하트비트로부터 HEARTBEAT_TIMEOUT_SECONDS 초가 넘었으면 연결을 끊습니다.
// ==========================================
void Session::StartHeartbeatCheck() {
    auto self(shared_from_this());
    heartbeat_timer_.expires_after(std::chrono::seconds(HEARTBEAT_CHECK_INTERVAL_SECONDS));
    heartbeat_timer_.async_wait([this, self](boost::system::error_code ec) {
        if (ec) {
            // timer가 취소됐거나(operation_aborted) 세션이 이미 종료된 경우
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_heartbeat_).count();

        if (elapsed > HEARTBEAT_TIMEOUT_SECONDS) {
            std::cerr << "[LoginServer] ⏰ 하트비트 타임아웃! 유저("
                << logged_in_id_ << ") " << elapsed << "초 동안 응답 없음. 연결 종료.\n";
            OnDisconnected();
            return;
        }

        // 아직 타임아웃 아님 → 다음 체크 예약
        StartHeartbeatCheck();
    });
}

void Session::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) return;

    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    uint16_t totalSize = sizeof(PacketHeader) + payloadSize;

    if (totalSize > MAX_PACKET_SIZE) {
        std::cerr << "🚨 [Error] 패킷 크기 초과! (PktID: " << pktId
            << ", Size: " << totalSize << " bytes) - 전송 취소\n";
        return;
    }

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
        [this, self, send_buf, pktId](boost::system::error_code ec, std::size_t) {
            if (ec) {
                // ★ [수정 2] 에러 핸들링: 단순 무시 대신 로그 출력
                std::cerr << "[LoginServer] ⚠️ 패킷 전송 실패 (PktID: " << pktId
                    << ", 유저: " << logged_in_id_ << "): " << ec.message() << "\n";
            }
        });
}

void Session::OnDisconnected() {
    // 타이머 취소 (OnDisconnected가 여러 번 호출되지 않도록)
    heartbeat_timer_.cancel();

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
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) {
                    // ★ [수정 2] 에러 핸들링: 잘못된 헤더 크기 로그
                    std::cerr << "[LoginServer] ⚠️ 잘못된 패킷 헤더 크기: " << header_.size
                        << " (유저: " << logged_in_id_ << ")\n";
                    return;
                }
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
            else {
                // ★ [수정 2] 에러 핸들링: 단순 OnDisconnected 대신 원인 로그 출력
                if (ec != boost::asio::error::eof && ec != boost::asio::error::connection_reset) {
                    std::cerr << "[LoginServer] ReadHeader 오류 (유저: " << logged_in_id_
                        << "): " << ec.message() << "\n";
                }
                OnDisconnected();
            }
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
            else {
                // ★ [수정 2] 에러 핸들링: 원인 로그 출력
                if (ec != boost::asio::error::eof && ec != boost::asio::error::connection_reset) {
                    std::cerr << "[LoginServer] ReadPayload 오류 (유저: " << logged_in_id_
                        << "): " << ec.message() << "\n";
                }
                OnDisconnected();
            }
        });
}
