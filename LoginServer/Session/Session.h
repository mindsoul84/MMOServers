#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <deque>
#include <utility>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "../LoginServer.h"
#include "../../Common/Network/PacketAssembler.h"
#include "../../Common/Define/LoginConstants.h"

struct SendBuffer; // 전방 선언

// ==========================================
// Session 클래스 선언 (Client -> Login)
//
//   Send()에 strand + send_queue 패턴 적용
//   DB 스레드의 post 콜백과 메인 스레드가 동시에 Send()를 호출할 경우
//   concurrent async_write가 발생하여 TCP 스트림이 오염될 수 있었음.
//   GatewaySession, ClientSession 등 다른 세션과 동일한 패턴으로 통일.
//
//   하드코딩된 Heartbeat 매직 넘버를 LoginConstants로 교체
//   std::vector<char> payload_buf_ -> PacketAssembler 고정 버퍼
// ==========================================
class Session : public std::enable_shared_from_this<Session> {
private:
    boost::asio::ip::tcp::socket socket_;

    //   strand + send_queue: concurrent async_write 방지
    boost::asio::io_context::strand strand_;
    std::deque<std::pair<std::shared_ptr<SendBuffer>, size_t>> send_queue_;

    PacketHeader header_;

    //   std::vector<char> payload_buf_ 제거 -> PacketAssembler로 교체
    PacketAssembler assembler_;

    std::string logged_in_id_ = "";

    // ==========================================
    //   Heartbeat 타임아웃 기반 끊김 감지
    //
    // 변경 전: PKT_CLIENT_SERVER_HEARTBEAT 수신은 로그 출력만 하고 끝 → 실질적인 끊김 감지 없음
    // 변경 후: last_heartbeat_ 갱신 + steady_timer로 주기 체크 → 타임아웃 시 강제 연결 종료
    //
    //   하드코딩된 매직 넘버(15, 30)를 LoginConstants로 교체
    // ==========================================
    boost::asio::steady_timer heartbeat_timer_;
    std::chrono::steady_clock::time_point last_heartbeat_;

public:
    Session(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void SetLoggedInId(const std::string& id) { logged_in_id_ = id; }
    const std::string& GetLoggedInId() const { return logged_in_id_; }
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

    //   하트비트 수신 시 Handle_Heartbeat()에서 호출
    void UpdateHeartbeat() {
        last_heartbeat_ = std::chrono::steady_clock::now();
    }

private:
    void OnDisconnected();
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
    void StartHeartbeatCheck();

    //   큐에서 패킷을 꺼내 실제로 전송하는 내부 함수
    void DoWrite();
};
