#pragma once
#include <memory>
#include <string>
#include <vector>
#include <deque> // 큐 사용

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include <boost/asio.hpp>
#include "../../Common/Protocol/protocol.pb.h"
#pragma warning(pop)

class StressManager;

// =========================================================
// 봇 AI 행동 확률 설정 (1~100 기준)
// =========================================================
inline constexpr const char* BOT_PASSWORD = "1234";

constexpr int BOT_ACTION_MOVE_PER = 70;         // 70% 확률 이동, 나머지(30%) 확률 공격
constexpr int BOT_MIN_ACTION_DELAY_MS = 2000;   // 최소 대기 시간 (밀리초)
constexpr int BOT_MAX_ACTION_DELAY_MS = 4000;   // 최대 대기 시간 (밀리초)

// 가상 유저의 현재 상태
enum class BotState {
    DISCONNECTED,
    CONNECTING_LOGIN,
    WAITING_LOGIN_RES,
    CONNECTING_GATEWAY,
    WAITING_GATEWAY_RES,
    IN_GAME
};

class StressManager;

class StressSession : public std::enable_shared_from_this<StressSession> {
private:
    boost::asio::ip::tcp::socket socket_;

    // 동시 접근 방지 & 전송 대기열
    boost::asio::io_context::strand strand_;
    std::deque<std::shared_ptr<std::vector<char>>> send_queue_;

    boost::asio::io_context& io_context_;
    boost::asio::steady_timer action_timer_; // 주기적 행동(이동/공격)을 위한 타이머

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

public:
    StressSession(boost::asio::io_context& io, StressManager* manager, const std::string& account_id);

    void Start();
    void Stop();

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