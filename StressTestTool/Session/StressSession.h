#pragma once
#include <memory>
#include <string>
#include <vector>
#include <deque>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include <boost/asio.hpp>
#include "../../Common/Protocol/protocol.pb.h"
#pragma warning(pop)

#include "../../Common/Define/StressConstants.h"
#include "../../Common/Network/PacketCrypto.h"

class StressManager;

enum class BotState {
    DISCONNECTED,
    CONNECTING_LOGIN,
    WAITING_LOGIN_RES,
    CONNECTING_GATEWAY,
    WAITING_GATEWAY_RES,
    IN_GAME
};

// ==========================================
// 패킷 암호화 통합
//
// GatewayConnectRes 성공 후 PacketCrypto를 초기화하여
// 이후 모든 Gateway 패킷을 AES-128-CBC로 암호화합니다.
// LoginServer 통신은 평문을 유지합니다.
// ==========================================
class StressSession : public std::enable_shared_from_this<StressSession> {
private:
    boost::asio::ip::tcp::socket socket_;

    // 동시 접근 방지 & 전송 대기열
    boost::asio::io_context::strand strand_;
    std::deque<std::shared_ptr<std::vector<char>>> send_queue_;

    boost::asio::io_context& io_context_;
    boost::asio::steady_timer action_timer_;

    BotState state_ = BotState::DISCONNECTED;

    // 유저 정보
    std::string account_id_;
    std::string session_token_;
    float x_ = 0.0f;
    float y_ = 0.0f;

    // 수신 버퍼
    struct PacketHeader {
        uint16_t size;
        uint16_t id;
    };
    PacketHeader header_;
    std::vector<char> payload_buf_;

    StressManager* manager_; // 통계 기록을 위해 매니저 포인터 보유

    // 세션별 패킷 암호화 (GatewayServer 통신 전용)
    PacketCrypto crypto_;
    bool crypto_enabled_ = false;

public:
    StressSession(boost::asio::io_context& io, StressManager* manager, const std::string& account_id);

    void Start();
    void Stop();

    // 봇 ID 재사용을 위한 Getter
    // 봇이 끊어졌을 때 StressManager가 이 ID를 재사용 풀에 반환합니다.
    const std::string& GetAccountId() const { return account_id_; }

private:
    // 네트워크 I/O
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
    void SendPacket(uint16_t pktId, const google::protobuf::Message& msg);

    // 패킷 핸들러
    void HandlePacket(uint16_t pktId, const char* payload, uint16_t size);

    // 비동기 봇 로직
    void ConnectToLogin();
    void ConnectToGateway(const std::string& ip, short port);
    void ScheduleNextAction(); // IN_GAME 상태에서 랜덤 이동/공격을 지시하는 AI 루프
    
    void DoWrite();     // 실제 비동기 전송을 수행하는 함수
};
