#include "GatewaySession.h"
#include "..\GameServer\GameServer.h"
#include "../../Common/MemoryPool.h"
#include "../../Common/Utils/Logger.h"
#include "../../Common/Redis/RedisManager.h"
#include <iostream>

using boost::asio::ip::tcp;

GatewaySession::GatewaySession(tcp::socket socket) noexcept
    : socket_(std::move(socket)), strand_(GameContext::Get().io_context) { }

void GatewaySession::start() {
    ReadHeader();
}

// ==========================================
// [수정] Send() - 메모리 풀 활용으로 통일
//
// 변경 전: make_shared<SendBuffer>(totalSize) -> 매번 힙 할당
//   -> 대규모 동접 시 new/delete가 초당 수만 회 발생하여 힙 할당자 병목 유발
//
// 변경 후: SendBufferPool에서 대여 -> 전송 완료 시 자동 반납 (SendBufferDeleter)
//   -> Lock-Free 풀에서 O(1)로 버퍼 획득, 힙 할당 비용 사실상 0
//   -> LoginServer, WorldServer, GatewayServer의 모든 Send 경로가 동일한 풀 패턴 사용
// ==========================================
void GatewaySession::Send(uint16_t pktId, const google::protobuf::Message& msg) {
    if (!socket_.is_open()) return;

    uint16_t payloadSize = static_cast<uint16_t>(msg.ByteSizeLong());
    uint16_t totalSize = sizeof(PacketHeader) + payloadSize;

    // 버퍼 크기 초과 시 서버 죽지 않도록 에러 로그만 띄우고 취소
    if (totalSize > MAX_PACKET_SIZE) {
        LOG_ERROR("GameServer", "패킷 크기 초과! (PktID: " << pktId
            << ", Size: " << totalSize << " bytes) - 전송 취소");
        return;
    }

    // [수정] 메모리 풀에서 버퍼 대여 (전송 완료 시 SendBufferDeleter가 자동 반납)
    SendBuffer* raw_buf = SendBufferPool::GetInstance().Acquire();
    std::shared_ptr<SendBuffer> send_buf(raw_buf, SendBufferDeleter());

    PacketHeader header{ totalSize, pktId };
    memcpy(send_buf->buffer_.data(), &header, sizeof(PacketHeader));
    msg.SerializeToArray(send_buf->buffer_.data() + sizeof(PacketHeader), payloadSize);

    auto self(shared_from_this());

    // [수정] 람다 캡처에 totalSize를 추가합니다.
    boost::asio::post(strand_, [this, self, send_buf, totalSize]() {
        // [Backpressure] 큐가 SEND_QUEUE_MAX_SIZE 이상 쌓이면 서버가 뻗지 않도록 패킷 드랍
        if (send_queue_.size() > GameConstants::Network::SEND_QUEUE_MAX_SIZE) {
            LOG_WARN("GameServer", "GatewaySession Send Queue 폭발! 전송 드랍.");
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
}

// [추가] 실제 비동기 전송을 수행하는 함수
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
                LOG_ERROR("GameServer", "Gateway로 S2S 패킷 전송 실패 (DoWrite)");
                send_queue_.clear();
            }
        }));
}

// ==========================================
// [수정] 게이트웨이 장애 시 유저 일괄 정리 (Fault Tolerance)
//
// 변경 전: gatewaySessions에서 자기 자신만 제거
//   -> 해당 게이트웨이를 통해 접속한 유저들이 playerMap에 좀비로 남음
//   -> Zone에서도 삭제되지 않아 AOI 브로드캐스트에 유령 유저 잔존
//
// 변경 후: game_strand_에 유저 일괄 정리 요청을 post
//   -> gatewayPlayerMap_에서 소속 유저 목록을 조회
//   -> playerMap, uidToAccount, Zone, Redis에서 일괄 삭제
//   -> 세션 라이프사이클(gatewaySessions)은 별도 뮤텍스로 즉시 정리
// ==========================================
void GatewaySession::OnDisconnected() {
    auto& ctx = GameContext::Get();
    auto self = shared_from_this();

    // game_strand_에서 게임 상태 정리 (유저 맵, Zone, Redis 등)
    boost::asio::post(ctx.game_strand_, [self]() {
        auto& ctx_inner = GameContext::Get();

        // 이 게이트웨이를 통해 접속한 모든 유저를 일괄 정리
        auto players = ctx_inner.GetPlayersOfGateway(self.get());
        for (const auto& acc_id : players) {
            auto it = ctx_inner.playerMap.find(acc_id);
            if (it != ctx_inner.playerMap.end()) {
                uint64_t uid = it->second->uid;
                float last_x = it->second->x;
                float last_y = it->second->y;

                ctx_inner.uidToAccount.erase(uid);
                ctx_inner.playerMap.erase(it);
                ctx_inner.zone->LeaveZone(uid, last_x, last_y);
                RedisManager::GetInstance().RemovePlayer(acc_id);

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
                if (acc_id.find("BOT_STRESS") != std::string::npos) {
                    ctx_inner.connected_bot_count.fetch_sub(1, std::memory_order_relaxed);
                }
#endif
            }
        }

        if (!players.empty()) {
            LOG_INFO("GameServer", "Gateway 장애 복구: " << players.size() << "명의 유저 일괄 정리 완료");
        }
        ctx_inner.RemoveGatewayMapping(self.get());
    });

    // 세션 라이프사이클 관리 (별도 뮤텍스 — game_strand_와 독립)
    {
        std::lock_guard<std::mutex> lock(ctx.gatewaySessionMutex);
        ctx.gatewaySessions.erase(self);
    }

    LOG_INFO("GameServer", "Gateway 연결 세션 자원 회수됨.");
}

// ==========================================
// [수정] ReadHeader - 패킷 디스패치를 game_strand_에 post
//
// 변경 전: 콜백이 실행되는 I/O 워커 스레드에서 직접 Dispatch 호출
//   -> 여러 워커 스레드에서 동시에 게임 상태 접근 가능
//   -> shared_mutex, PlayerInfo::mtx 등 복잡한 락 체계 필요
//
// 변경 후: Dispatch를 game_strand_에 post하여 게임 로직 직렬화
//   -> 네트워크 I/O 스레드는 패킷 읽기만 수행
//   -> 게임 로직은 game_strand_에서 단일 스레드로 실행
//   -> 락 불필요, 데드락 불가능
// ==========================================
void GatewaySession::ReadHeader() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                if (header_.size < sizeof(PacketHeader) || header_.size > MAX_PACKET_SIZE) return;
                uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));

                if (payload_size == 0) {
                    // [수정] game_strand_에 디스패치하여 게임 로직 직렬화
                    uint16_t pkt_id = header_.id;
                    boost::asio::post(GameContext::Get().game_strand_, [self, pkt_id]() {
                        auto session_ptr = self;
                        GameContext::Get().gatewayDispatcher.Dispatch(session_ptr, pkt_id, nullptr, 0);
                    });
                    ReadHeader();
                }
                else {
                    payload_buf_.resize(payload_size);
                    ReadPayload(payload_size);
                }
            }
            else {
                LOG_INFO("GameServer", "GatewayServer와의 S2S 연결 해제됨.");
                OnDisconnected();
            }
        });
}

// ==========================================
// [수정] ReadPayload - 페이로드 복사 후 game_strand_에 post
//
// ReadHeader() 이후 즉시 다음 패킷을 읽기 시작하므로
// payload_buf_가 덮어쓰이기 전에 복사본을 만들어야 합니다.
// 복사본은 shared_ptr<vector<char>>로 game_strand_ 람다에 캡처됩니다.
// ==========================================
void GatewaySession::ReadPayload(uint16_t payload_size) {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
        [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                // 페이로드 복사 (다음 ReadHeader가 payload_buf_를 덮어쓰기 전)
                auto payload_copy = std::make_shared<std::vector<char>>(
                    payload_buf_.begin(), payload_buf_.begin() + payload_size);
                uint16_t pkt_id = header_.id;

                // game_strand_에 디스패치하여 게임 로직 직렬화
                boost::asio::post(GameContext::Get().game_strand_,
                    [self, pkt_id, payload_copy, payload_size]() {
                        auto session_ptr = self;
                        GameContext::Get().gatewayDispatcher.Dispatch(
                            session_ptr, pkt_id, payload_copy->data(), payload_size);
                    });

                ReadHeader();
            }
            else {
                LOG_ERROR("GameServer", "GatewayServer와의 연결이 끊어졌습니다.");
                OnDisconnected();
            }
        });
}
