#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <deque>
#include <utility>
#include <string>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "../LoginServer.h"

// ==========================================
// WorldConnection: 재연결(Reconnect) 로직 추가
//
// [변경 전] 동기 연결만 지원. 연결 끊어지면 서버 전체 기능 상실
//   -> WorldServer 장애 시 LoginServer도 사실상 죽음
//   -> 월드 선택 기능 완전 불가
//
// [변경 후] 비동기 연결 + 지수 백오프(Exponential Backoff) 재연결
//   -> GatewayServer의 GameConnection과 동일한 패턴 적용
//   -> WorldServer 재시작 시 자동 재연결
//   -> 연결 끊김 시 3초 간격으로 재연결 시도
//   -> 재연결 전 send_queue 초기화로 잔여 패킷 제거
// ==========================================
class WorldConnection : public std::enable_shared_from_this<WorldConnection> {
private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context& io_context_;

    // strand + send_queue: concurrent async_write 방지
    boost::asio::io_context::strand strand_;
    std::deque<std::pair<std::shared_ptr<std::vector<char>>, size_t>> send_queue_;

    //   재연결 인프라
    boost::asio::steady_timer retry_timer_;
    std::string target_ip_;
    short target_port_ = 0;
    bool connected_ = false;

    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    WorldConnection(boost::asio::io_context& io_context);
    void Connect(const std::string& ip, short port);
    void Send(uint16_t pktId, const google::protobuf::Message& msg);
    bool IsConnected() const { return connected_; }

private:
    void DoConnect();       // 비동기 연결 시도
    void ScheduleRetry();   // 재연결 예약 (3초 후)
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
    void DoWrite();
};
