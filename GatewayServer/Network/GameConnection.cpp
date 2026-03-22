#include "GameConnection.h"
#include "..\Common\MemoryPool.h"
#include <iostream>
#include <string>

using boost::asio::ip::tcp;


// ==========================================
// GameConnection 클래스 구현부
// ==========================================
GameConnection::GameConnection(boost::asio::io_context& io_context)
    : socket_(io_context), io_context_(io_context), retry_timer_(io_context) {
}

void GameConnection::Connect(const std::string& ip, short port) {
    target_ip_ = ip;
    target_port_ = port;
    DoConnect();
}

void GameConnection::Send(uint16_t pktId, const google::protobuf::Message& msg) {
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
        [this, self, send_buf](boost::system::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "[Gateway] GameServer로 S2S 패킷 전송 실패\n";
            }
        });

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
        [this, self, send_buf](boost::system::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "[Gateway] GameServer로 S2S 패킷 전송 실패\n";
            }
        });

#endif//DEF_STRESS_TEST_TOOL 


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
                    // ★ [수정] g_game_dispatcher → GatewayContext::Get().gameDispatcher
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
                // ★ [수정] g_game_dispatcher → GatewayContext::Get().gameDispatcher
                GatewayContext::Get().gameDispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else {
                std::cerr << "🚨 [Gateway] GameServer와의 연결이 끊어졌습니다!\n";
                ScheduleRetry();
            }
        });
}
