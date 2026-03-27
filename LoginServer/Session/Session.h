#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include <chrono>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "../LoginServer.h"

// ==========================================
// ★ Session 클래스 선언 (Client -> Login)
// ==========================================
class Session : public std::enable_shared_from_this<Session> {
private:
    boost::asio::ip::tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;
    std::string logged_in_id_ = "";

    // ==========================================
    // [수정] Heartbeat 타임아웃 기반 끊김 감지
    //
    // 변경 전: PKT_CLIENT_SERVER_HEARTBEAT 수신은 로그 출력만 하고 끝 → 실질적인 끊김 감지 없음
    // 변경 후: last_heartbeat_ 갱신 + steady_timer로 주기 체크 → 타임아웃 시 강제 연결 종료
    //
    // 흐름:
    //   start() → StartHeartbeatCheck() 타이머 시작
    //   Handle_Heartbeat() 수신 → UpdateHeartbeat()로 last_heartbeat_ 갱신
    //   타이머 만료 → now - last_heartbeat_ > HEARTBEAT_TIMEOUT_SECONDS → OnDisconnected()
    // ==========================================
    boost::asio::steady_timer heartbeat_timer_;
    std::chrono::steady_clock::time_point last_heartbeat_;

    // 하트비트 체크 주기(초): 이 간격으로 타임아웃 여부를 확인
    static constexpr int HEARTBEAT_CHECK_INTERVAL_SECONDS = 15;
    // 클라이언트가 이 시간(초) 동안 하트비트를 보내지 않으면 연결 종료
    static constexpr int HEARTBEAT_TIMEOUT_SECONDS = 30;

public:
    Session(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void SetLoggedInId(const std::string& id) { logged_in_id_ = id; }
    const std::string& GetLoggedInId() const { return logged_in_id_; }
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

    // ★ [추가 - 수정] 하트비트 수신 시 Handle_Heartbeat()에서 호출
    void UpdateHeartbeat() {
        last_heartbeat_ = std::chrono::steady_clock::now();
    }

private:
    void OnDisconnected();
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);

    // ★ [추가 - 수정] 타이머 기반 하트비트 타임아웃 체크 시작
    void StartHeartbeatCheck();
};
