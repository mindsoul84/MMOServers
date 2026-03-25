#include "GatewaySession.h"
#include "..\GameServer\GameServer.h"
#include "../../Common/MemoryPool.h"
#include <iostream>

using boost::asio::ip::tcp;

GatewaySession::GatewaySession(tcp::socket socket) noexcept
    : socket_(std::move(socket)), strand_(GameContext::Get().io_context) { }

void GatewaySession::start() {
    ReadHeader();
}

void GatewaySession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) return;

#ifdef  DEF_STRESS_TEST_TOOL
    // =========================================================
    // 🚀 [부하 테스트 전용 모드] 메모리 폭발 방지를 위한 가변 크기 버퍼
    // =========================================================
    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    uint16_t totalSize = sizeof(PacketHeader) + payloadSize;

    // 버퍼 크기 초과 시 서버 죽지 않도록 에러 로그만 띄우고 취소
    if (totalSize > MAX_PACKET_SIZE) {
        std::cerr << "🚨 [Error] 패킷 크기 초과! (PktID: " << pktId
            << ", Size: " << totalSize << " bytes) - 전송 취소\n";
        return;
    }

    // ★ 핵심: MemoryPool을 거치지 않고, 가변 길이 생성자(exact_size)를 사용하여 딱 필요한 만큼만 할당!
    auto send_buf = std::make_shared<SendBuffer>(totalSize);

    PacketHeader header{ totalSize, pktId };
    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    msg.SerializeToArray(send_buf->buffer_.data() + sizeof(PacketHeader), payloadSize);

    auto self(shared_from_this());

    // ★ [수정] 람다 캡처에 totalSize를 추가합니다.
    boost::asio::post(strand_, [this, self, send_buf, totalSize]() {
        // [Backpressure] 큐가 SEND_QUEUE_MAX_SIZE 이상 쌓이면 서버가 뻗지 않도록 패킷 드랍
        if (send_queue_.size() > GameConstants::Network::SEND_QUEUE_MAX_SIZE) {
            std::cerr << "🚨 [Warning] GatewaySession Send Queue 폭발! 전송 드랍.\n";
            return;
        }

        bool write_in_progress = !send_queue_.empty();

        // =========================================================
        // 큐의 형식에 맞게 버퍼와 사이즈를 pair로 넣습니다! (emplace_back 사용)
        // =========================================================
        send_queue_.emplace_back(send_buf, totalSize);

        if (!write_in_progress) {
            DoWrite();
        }
    });

#else//DEF_STRESS_TEST_TOOL

    std::string payload;
    msg.SerializeToString(&payload);
    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
    header.id = pktId;

    // MemoryPool을 활용한 락프리 버퍼 할당
    SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();
    std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());

    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    memcpy(send_buf->buffer_.data() + sizeof(PacketHeader), payload.data(), payload.size());

    auto self(shared_from_this());

    size_t write_size = header.size;

    // =========================================================
    // ★ [핵심] 여러 스레드가 동시에 Send를 호출해도, strand_ 내부에서 안전하게 큐에 쌓입니다.
    // =========================================================
    boost::asio::post(strand_, [this, self, send_buf, write_size]() {
        // 큐가 비어있었다면, 현재 소켓이 놀고 있다는 뜻이므로 즉시 전송을 시작합니다.
        bool write_in_progress = !send_queue_.empty();

        send_queue_.push_back({ send_buf, write_size });

        if (!write_in_progress) {
            DoWrite();
        }
    });

#endif//DEF_STRESS_TEST_TOOL
}

// ★ [추가] 실제 비동기 전송을 수행하는 함수
void GatewaySession::DoWrite() {
    auto self(shared_from_this());
    auto& front_msg = send_queue_.front();

    // bind_executor를 사용하여 콜백 또한 strand_ 내부에서 안전하게 실행되도록 보장합니다.
    boost::asio::async_write(socket_,
        boost::asio::buffer(front_msg.first->buffer_.data(), front_msg.second),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (!ec) {
                // 방금 전송이 끝난 패킷을 큐에서 제거합니다.
                send_queue_.pop_front();

                // 큐에 대기 중인 다음 패킷이 있다면 이어서 전송합니다. (꼬리물기)
                if (!send_queue_.empty()) {
                    DoWrite();
                }
            }
            else {
                // 에러 발생 시 큐를 비워버립니다. 소켓 정리는 Read 쪽에서 처리됩니다.
                std::cerr << "[GameServer] Gateway로 S2S 패킷 전송 실패 (DoWrite)\n";
                send_queue_.clear();
            }
        }));
}

// ★ [추가] 세션 종료 시 메모리 누수를 방지 중앙 함수
void GatewaySession::OnDisconnected() {
    auto& ctx = GameContext::Get();
    std::lock_guard<std::mutex> lock(ctx.gatewaySessionMutex);

    // std::unordered_set을 사용하므로 std::find 없이 즉시 O(1)로 삭제됩니다!
    // shared_from_this()를 지워주어야 레퍼런스 카운트가 0이 되어 좀비 세션이 사라집니다.
    size_t removed = ctx.gatewaySessions.erase(shared_from_this());

    if (removed > 0) {
        std::cout << "[GameServer] ♻️ Gateway 연결 세션 자원 및 메모리가 안전하게 회수되었습니다.\n";
    }
}

void GatewaySession::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) return;
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));

                if (payload_size == 0) {
                    auto session_ptr = self;
                    // ★ [수정] GameContext의 디스패처 사용
                    GameContext::Get().gatewayDispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                    ReadHeader();
                }
                else {
                    payload_buf_.resize(payload_size);
                    ReadPayload(payload_size);
                }
            }
            else {
                std::cout << "[GameServer] GatewayServer와의 S2S 연결 해제됨.\n";
                // ★ [수정] 삭제 로직 대신 함수 호출
                OnDisconnected();
            }
        });
}

void GatewaySession::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                auto session_ptr = self;
                // GameContext의 디스패처 사용
                GameContext::Get().gatewayDispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                ReadHeader();
            }
            else {
                std::cerr << "[GameServer] 🚨 GatewayServer와의 연결이 끊어졌습니다.\n";
                // ★ [핵심 수정] 기존에 누락되어 메모리 누수(좀비 세션) 해결!
                OnDisconnected();
            }

        });
}