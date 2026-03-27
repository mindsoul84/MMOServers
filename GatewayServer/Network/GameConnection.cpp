#include "GameConnection.h"
#include "..\Common\MemoryPool.h"
#include <iostream>
#include <string>

using boost::asio::ip::tcp;

GameConnection::GameConnection(boost::asio::io_context& io_context)
    : socket_(io_context)
    , io_context_(io_context)
    , retry_timer_(io_context)
    , strand_(io_context)   // ★ [버그 픽스] strand_ 초기화 추가
{}

void GameConnection::Connect(const std::string& ip, short port) {
    target_ip_ = ip;
    target_port_ = port;
    DoConnect();
}

// ==========================================
// ★ [버그 픽스] Send() 전면 재작성 - strand + send_queue 패턴 적용
//
// 변경 전:
//   boost::asio::async_write(socket_, ...) 를 strand 없이 직접 호출
//   → StressTestTool 3000봇 동시 종료 시 3000번의 concurrent async_write 발생
//   → 소켓 TCP 스트림 오염 → LEAVE_REQ 패킷 유실 → GameServer Zone 삭제 안 됨
//
// 변경 후:
//   boost::asio::post(strand_, ...) 로 큐에 쌓은 뒤 DoWrite()로 순차 전송
//   → GatewaySession, ClientSession과 동일한 패턴으로 스레드 안전성 확보
// ==========================================
void GameConnection::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) return;

    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    uint16_t totalSize = sizeof(PacketHeader) + payloadSize;

    if (totalSize > MAX_PACKET_SIZE) {
        std::cerr << "🚨 [Error] 패킷 크기 초과! (PktID: " << pktId
            << ", Size: " << totalSize << " bytes) - 전송 취소\n";
        return;
    }

    auto send_buf = std::make_shared<std::vector<char>>(totalSize);
    PacketHeader header{ totalSize, pktId };
    memcpy(send_buf->data(), &header, sizeof(PacketHeader));
    msg.SerializeToArray(send_buf->data() + sizeof(PacketHeader), payloadSize);

    auto self(shared_from_this());

    // ★ [버그 픽스] strand_ 내부에서 큐에 쌓고 DoWrite()로 순차 전송
    boost::asio::post(strand_, [this, self, send_buf, totalSize]() {
        bool write_in_progress = !send_queue_.empty();
        send_queue_.emplace_back(send_buf, static_cast<size_t>(totalSize));
        if (!write_in_progress) DoWrite();
    });
}

// ==========================================
// ★ [버그 픽스] 실제 비동기 전송을 수행하는 함수 (신규 추가)
//
// strand_ 내부에서만 호출되며, 큐의 맨 앞 패킷부터 순차적으로 전송합니다.
// 전송 완료 콜백도 bind_executor(strand_)로 보호하여 스레드 안전성을 확보합니다.
// ==========================================
void GameConnection::DoWrite() {
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
                std::cerr << "[Gateway] GameServer로 S2S 패킷 전송 실패 (DoWrite): " << ec.message() << "\n";
                send_queue_.clear();
                ScheduleRetry();
            }
        }));
}

void GameConnection::DoConnect() {
    try {
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(target_ip_, std::to_string(target_port_));

        auto self(shared_from_this());
        boost::asio::async_connect(socket_, endpoints,
            [this, self](boost::system::error_code ec, tcp::endpoint) {
                if (!ec) {
                    std::cout << "[Gateway] 🕹️ GameServer(S2S) 9000번 포트에 성공적으로 연결되었습니다!\n";
                    ReadHeader();
                }
                else {
                    std::cerr << "🚨 [Gateway] GameServer가 꺼져 있습니다. 3초 후 재연결 시도...\n";
                    ScheduleRetry();
                }
            });
    }
    catch (std::exception& e) {
        std::cerr << "[Gateway] 주소 변환 에러: " << e.what() << "\n";
        ScheduleRetry();
    }
}

void GameConnection::ScheduleRetry() {
    // ★ 재연결 전 send_queue 초기화 (이전 연결의 잔여 패킷 제거)
    boost::asio::post(strand_, [this]() { send_queue_.clear(); });

    if (socket_.is_open()) {
        boost::system::error_code ec;
        socket_.close(ec);
    }

    auto self(shared_from_this());
    retry_timer_.expires_after(std::chrono::seconds(3));
    retry_timer_.async_wait([this, self](boost::system::error_code ec) {
        if (!ec) {
            std::cout << "[Gateway] GameServer 재연결 시도 중...\n";
            DoConnect();
        }
    });
}

void GameConnection::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) return;
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                if (payload_size == 0) {
                    auto session_ptr = self;
                    // [수정] g_game_dispatcher → GatewayContext::Get().gameDispatcher
                    GatewayContext::Get().gameDispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                    ReadHeader();
                }
                else {
                    payload_buf_.resize(payload_size);
                    ReadPayload(payload_size);
                }
            }
            else {
                std::cerr << "🚨 [Gateway] GameServer와의 연결이 끊어졌습니다!\n";
                ScheduleRetry();
            }
        });
}

void GameConnection::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                // [수정] g_game_dispatcher → GatewayContext::Get().gameDispatcher
                GatewayContext::Get().gameDispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else {
                std::cerr << "🚨 [Gateway] GameServer와의 연결이 끊어졌습니다!\n";
                ScheduleRetry();
            }
        });
}
