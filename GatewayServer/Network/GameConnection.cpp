#include "GameConnection.h"
#include "..\Common\MemoryPool.h"
#include <iostream>
#include <string>

using boost::asio::ip::tcp;

GameConnection::GameConnection(boost::asio::io_context& io_context)
    : socket_(io_context)
    , io_context_(io_context)
    , retry_timer_(io_context)
    , strand_(io_context)   // strand_ 초기화 추가
{}

void GameConnection::Connect(const std::string& ip, short port) {
    target_ip_ = ip;
    target_port_ = port;
    DoConnect();
}

// ==========================================
// Send() 전면 재작성 - strand + send_queue 패턴 적용
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

    boost::asio::post(strand_, [this, self, send_buf, totalSize]() {
        bool write_in_progress = !send_queue_.empty();
        send_queue_.emplace_back(send_buf, static_cast<size_t>(totalSize));
        if (!write_in_progress) DoWrite();
    });
}

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

// ==========================================
//   ScheduleRetry - 수신 상태 초기화 추가
//
// 변경 전: send_queue만 초기화하고 소켓을 닫음
//   -> 이전 연결에서 수신 중이던 패킷의 잔여 상태(header_, payload_buf_)가
//      다음 연결의 ReadHeader에서 참조될 수 있음
//   -> 특히 header_에 이전 연결의 부분 수신 데이터가 남아있으면
//      새 연결의 첫 패킷 파싱이 오염됨
//
// 변경 후: header_와 payload_buf_를 명시적으로 초기화하여
//   새 연결이 깨끗한 수신 상태에서 시작하도록 보장
// ==========================================
void GameConnection::ScheduleRetry() {
    // 전송 큐 초기화 (이전 연결의 잔여 패킷 제거)
    boost::asio::post(strand_, [this]() { send_queue_.clear(); });

    //   수신 상태 초기화 — 이전 연결의 잔여 데이터 제거
    std::memset(&header_, 0, sizeof(PacketHeader));
    payload_buf_.clear();

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

// ==========================================
// [참고] boost::asio::async_read의 완전 수신 보장
//
// boost::asio::async_read는 요청한 바이트 수를 모두 수신할 때까지
// 내부적으로 반복하여 async_read_some을 호출합니다.
// 따라서 TCP 스트림에서 PacketHeader(4바이트)가 여러 세그먼트로
// 분할 수신되더라도, 콜백은 반드시 4바이트를 모두 받은 후에만 호출됩니다.
// 이는 boost::asio의 composed operation 보장 사항입니다.
// (ref: boost.org/doc/libs/release/doc/html/boost_asio/reference/async_read.html)
// ==========================================
void GameConnection::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) return;
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                if (payload_size == 0) {
                    auto session_ptr = self;
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
                GatewayContext::Get().gameDispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else {
                std::cerr << "🚨 [Gateway] GameServer와의 연결이 끊어졌습니다!\n";
                ScheduleRetry();
            }
        });
}
