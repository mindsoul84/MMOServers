#include "WorldConnection.h"
#include "..\GameServer\GameServer.h"
#include "../../Common/MemoryPool.h"
#include <iostream>

using boost::asio::ip::tcp;

WorldConnection::WorldConnection(boost::asio::io_context& io_context)
    : socket_(io_context), io_context_(io_context) {
}

void WorldConnection::Connect(const std::string& ip, short port) {
    tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(ip, std::to_string(port));

    auto self(shared_from_this());
    boost::asio::async_connect(socket_, endpoints,
        [this, self](boost::system::error_code ec, tcp::endpoint) {
            if (!ec) {
                try {
                    unsigned short my_port = socket_.local_endpoint().port();
                    std::cout << "[GameServer] WorldServer(7000)와 성공적으로 연결되었습니다! (내 부여된 포트: " << my_port << ")\n";
                }
                catch (...) {
                    std::cout << "[GameServer] WorldServer(7000)와 성공적으로 연결되었습니다!\n";
                }
                ReadHeader();
            }
            else {
                std::cerr << "🚨 [GameServer] WorldServer 연결 실패\n";
            }
        });
}

void WorldConnection::Send(uint16_t pktId, const google::protobuf::Message& msg) {
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
            if (ec) std::cerr << "[GameServer] WorldServer로 패킷 전송 실패\n";
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
            if (ec) std::cerr << "[GameServer] WorldServer로 패킷 전송 실패\n";
        });

#endif//DEF_STRESS_TEST_TOOL 
}

void WorldConnection::ReadHeader() {
    auto self(shared_from_this());

    // 내부 람다에서 사용할 임시 헤더 버퍼 (동적 할당 피하기 위해 공유 포인터 사용)
    auto header_buf = std::make_shared<PacketHeader>();

    boost::asio::async_read(socket_, boost::asio::buffer(header_buf.get(), sizeof(PacketHeader)),
        [this, self, header_buf](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_buf->size < sizeof(PacketHeader) || header_buf->size > 4096) return;

                current_header_id_ = header_buf->id;
                current_payload_size_ = static_cast<uint16_t>(header_buf->size - sizeof(PacketHeader));

                if (current_payload_size_ == 0) {
                    auto session_ptr = self;
                    // ★ [수정] GameContext의 월드 디스패처 사용
                    GameContext::Get().worldDispatcher.Dispatch(session_ptr, current_header_id_, nullptr, 0);
                    ReadHeader();
                }
                else {
                    payload_buf_.resize(current_payload_size_);
                    ReadPayload();
                }
            }
            else {
                std::cerr << "[GameServer] 🚨 WorldServer와의 연결이 끊어졌습니다.\n";
            }
        });
}

void WorldConnection::ReadPayload() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), current_payload_size_),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                // ★ [수정] GameContext의 월드 디스패처 사용
                GameContext::Get().worldDispatcher.Dispatch(session_ptr, current_header_id_, payload_buf_.data(), current_payload_size_);
                ReadHeader();
            }
            else {
                std::cerr << "[GameServer] 🚨 WorldServer와의 연결이 끊어졌습니다.\n";
            }
        });
}