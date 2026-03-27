#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <utility>
#include <google/protobuf/message.h>
#include "..\GatewayServer.h"
#include "..\..\Common\PacketDispatcher.h"

struct SendBuffer;

// ==========================================
// [패킷 파이프라인 강화] Rate Limiter 추가
//
// 변경 전: 클라이언트가 보내는 패킷 수에 제한 없음
//   -> 악의적 클라이언트가 초당 수만 패킷 전송 시 서버 자원 고갈
//
// 변경 후: PacketRateLimiter로 초당 패킷 수 제한
//   -> 초과 시 패킷 드랍 + 경고 로그
//   -> 반복 초과 시 연결 강제 종료 가능 (violation_count_ 추적)
// ==========================================
class ClientSession : public std::enable_shared_from_this<ClientSession> {
private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context::strand strand_;
    std::deque<std::pair<std::shared_ptr<SendBuffer>, size_t>> send_queue_;

    PacketHeader header_;
    std::vector<char> payload_buf_;
    std::string account_id_ = "";

    // [패킷 파이프라인] 세션별 Rate Limiter
    PacketRateLimiter rate_limiter_;
    int rate_violation_count_ = 0;          // 연속 초과 횟수
    static constexpr int MAX_VIOLATIONS = 5; // 이 횟수 초과 시 강제 연결 종료

public:
    ClientSession(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void SetAccountId(const std::string& id);
    const std::string& GetAccountId() const;
    void Send(uint16_t pktId, const google::protobuf::Message& msg);
    void OnDisconnected();

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
    void DoWrite();
};
