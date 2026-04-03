#include "WorldConnection.h"
#include "..\GameServer\GameServer.h"
#include "../../Common/MemoryPool.h"
#include "../../Common/Utils/Logger.h"
#include <iostream>

using boost::asio::ip::tcp;

WorldConnection::WorldConnection(boost::asio::io_context& io_context)
    : socket_(io_context)
    , io_context_(io_context)
    , strand_(io_context)   // strand_ 초기화
{}

void WorldConnection::Connect(const std::string& ip, short port) {
    tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(ip, std::to_string(port));

    auto self(shared_from_this());
    boost::asio::async_connect(socket_, endpoints,
        [this, self](boost::system::error_code ec, tcp::endpoint) {
            if (!ec) {
                try {
                    unsigned short my_port = socket_.local_endpoint().port();
                    LOG_INFO("GameServer", "WorldServer(7000)와 성공적으로 연결되었습니다! (내 부여된 포트: " << my_port << ")");
                }
                catch (...) {
                    LOG_INFO("GameServer", "WorldServer(7000)와 성공적으로 연결되었습니다!");
                }
                ReadHeader();
            }
            else {
                LOG_ERROR("GameServer", "WorldServer 연결 실패");
            }
        });
}

// ==========================================
// Send() - strand + send_queue 패턴 적용
// ==========================================
void WorldConnection::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) return;

    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    uint16_t totalSize = sizeof(PacketHeader) + payloadSize;

    if (totalSize > MAX_PACKET_SIZE) {
        LOG_ERROR("GameServer", "패킷 크기 초과! (PktID: " << pktId
            << ", Size: " << totalSize << " bytes) - 전송 취소");
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

void WorldConnection::DoWrite() {
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
                LOG_ERROR("GameServer", "WorldServer로 패킷 전송 실패 (DoWrite): " << ec.message());
                send_queue_.clear();
            }
        }));
}

// ==========================================
// [참고] boost::asio::async_read의 완전 수신 보장
//
// boost::asio::async_read는 composed operation으로,
// 요청한 바이트 수를 모두 수신할 때까지 내부적으로
// async_read_some을 반복 호출합니다.
// TCP 스트림에서 PacketHeader(4바이트)가 분할 수신되더라도,
// 콜백은 4바이트를 모두 받은 후에만 호출됩니다.
// ==========================================
void WorldConnection::ReadHeader() {
    auto self(shared_from_this());

    auto header_buf = std::make_shared<PacketHeader>();

    boost::asio::async_read(socket_, boost::asio::buffer(header_buf.get(), sizeof(PacketHeader)),
        [this, self, header_buf](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_buf->size < sizeof(PacketHeader) || header_buf->size > 4096) return;

                uint16_t pkt_id = header_buf->id;
                uint16_t payload_size = static_cast<uint16_t>(header_buf->size - sizeof(PacketHeader));

                if (payload_size == 0) {
                    //   game_strand_에 디스패치
                    boost::asio::post(GameContext::Get().game_strand_, [self, pkt_id]() {
                        auto session_ptr = self;
                        GameContext::Get().worldDispatcher.Dispatch(session_ptr, pkt_id, nullptr, 0);
                    });
                    ReadHeader();
                }
                else {
                    // pkt_id와 payload_size를 멤버 변수에 저장 (ReadPayload에서 사용)
                    current_header_id_ = pkt_id;
                    current_payload_size_ = payload_size;
                    payload_buf_.resize(payload_size);
                    ReadPayload();
                }
            }
            else {
                LOG_ERROR("GameServer", "WorldServer와의 연결이 끊어졌습니다.");
            }
        });
}

void WorldConnection::ReadPayload() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), current_payload_size_),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                //   페이로드 복사 후 game_strand_에 디스패치
                auto payload_copy = std::make_shared<std::vector<char>>(
                    payload_buf_.begin(), payload_buf_.begin() + current_payload_size_);
                uint16_t pkt_id = current_header_id_;
                uint16_t pay_size = current_payload_size_;

                boost::asio::post(GameContext::Get().game_strand_,
                    [self, pkt_id, payload_copy, pay_size]() {
                        auto session_ptr = self;
                        GameContext::Get().worldDispatcher.Dispatch(
                            session_ptr, pkt_id, payload_copy->data(), pay_size);
                    });

                ReadHeader();
            }
            else {
                LOG_ERROR("GameServer", "WorldServer와의 연결이 끊어졌습니다.");
            }
        });
}
