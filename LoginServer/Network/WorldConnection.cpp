#include "WorldConnection.h"
#include "../../Common/Utils/Logger.h"
#include <iostream>

using boost::asio::ip::tcp;

WorldConnection::WorldConnection(boost::asio::io_context& io_context)
    : socket_(io_context)
    , io_context_(io_context)
    , strand_(io_context)
    , retry_timer_(io_context)
{}

// ==========================================
// [에러 복구] 동기 -> 비동기 연결로 전환
//
// 변경 전: boost::asio::connect() 동기 호출
//   -> WorldServer가 꺼져 있으면 LoginServer도 즉시 예외로 종료
//
// 변경 후: async_connect + ScheduleRetry 패턴
//   -> WorldServer 부재 시 3초 간격으로 재연결 시도
//   -> LoginServer는 WorldServer 없이도 기동 가능 (월드 선택만 불가)
// ==========================================
void WorldConnection::Connect(const std::string& ip, short port) {
    target_ip_ = ip;
    target_port_ = port;
    DoConnect();
}

void WorldConnection::DoConnect() {
    try {
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(target_ip_, std::to_string(target_port_));

        auto self(shared_from_this());
        boost::asio::async_connect(socket_, endpoints,
            [this, self](boost::system::error_code ec, tcp::endpoint) {
                if (!ec) {
                    connected_ = true;
                    try {
                        unsigned short my_port = socket_.local_endpoint().port();
                        LOG_INFO("LoginServer", "WorldServer(S2S)에 성공적으로 연결되었습니다! (부여 포트: " << my_port << ")");
                    }
                    catch (...) {
                        LOG_INFO("LoginServer", "WorldServer(S2S)에 성공적으로 연결되었습니다!");
                    }
                    ReadHeader();
                }
                else {
                    LOG_WARN("LoginServer", "WorldServer 연결 실패. 3초 후 재연결 시도...");
                    ScheduleRetry();
                }
            });
    }
    catch (std::exception& e) {
        LOG_ERROR("LoginServer", "WorldServer 주소 변환 에러: " << e.what());
        ScheduleRetry();
    }
}

// ==========================================
//   ScheduleRetry - 수신 상태 초기화 추가
//
// 변경 전: send_queue와 소켓만 정리
//   -> header_와 payload_buf_에 이전 연결의 잔여 데이터가 남을 수 있음
//
// 변경 후: header_와 payload_buf_를 명시적으로 초기화하여
//   새 연결이 깨끗한 수신 상태에서 시작하도록 보장
// ==========================================
void WorldConnection::ScheduleRetry() {
    connected_ = false;

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
            LOG_INFO("LoginServer", "WorldServer 재연결 시도 중...");
            DoConnect();
        }
    });
}

void WorldConnection::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!connected_ || !socket_.is_open()) {
        LOG_WARN("LoginServer", "WorldServer 미연결 상태에서 Send 시도 (PktID: " << pktId << ") - 패킷 드랍");
        return;
    }

    std::string payload;
    msg.SerializeToString(&payload);
    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
    header.id = pktId;

    auto send_buf = std::make_shared<std::vector<char>>(header.size);
    memcpy(send_buf->data(), &header, sizeof(PacketHeader));
    memcpy(send_buf->data() + sizeof(PacketHeader), payload.data(), payload.size());

    size_t write_size = header.size;
    auto self(shared_from_this());

    boost::asio::post(strand_, [this, self, send_buf, write_size]() {
        bool write_in_progress = !send_queue_.empty();
        send_queue_.emplace_back(send_buf, write_size);
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
                LOG_ERROR("LoginServer", "WorldServer로 패킷 전송 실패 (DoWrite): " << ec.message());
                send_queue_.clear();
                ScheduleRetry();
            }
        }));
}

// ==========================================
// [참고] boost::asio::async_read의 완전 수신 보장
//
// boost::asio::async_read는 요청한 바이트 수를 모두 수신할 때까지
// 내부적으로 async_read_some을 반복 호출합니다.
// TCP 스트림에서 PacketHeader(4바이트)가 분할 수신되더라도,
// 콜백은 4바이트를 모두 받은 후에만 호출됩니다.
// (boost::asio composed operation guarantee)
// ==========================================
void WorldConnection::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                if (payload_size == 0) {
                    auto session_ptr = self;
                    g_world_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                    ReadHeader();
                }
                else {
                    payload_buf_.resize(payload_size);
                    ReadPayload(payload_size);
                }
            }
            else {
                LOG_WARN("LoginServer", "WorldServer와의 연결이 끊어졌습니다! 재연결 시도...");
                ScheduleRetry();
            }
        });
}

void WorldConnection::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                g_world_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else {
                LOG_WARN("LoginServer", "WorldServer와의 연결이 끊어졌습니다! 재연결 시도...");
                ScheduleRetry();
            }
        });
}
