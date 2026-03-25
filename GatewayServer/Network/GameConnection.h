#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <utility>
#include <google/protobuf/message.h>
#include "../GatewayServer.h"

class GameConnection : public std::enable_shared_from_this<GameConnection> {
private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context& io_context_;
    boost::asio::steady_timer retry_timer_;

    // ==========================================
    // ★ [버그 픽스] strand_ + send_queue_ 추가
    //
    // 변경 전: async_write를 strand 없이 직접 호출
    //   → StressTestTool 3000봇이 동시에 끊어질 때 3000번의 concurrent async_write 발생
    //   → 같은 소켓에 동시 쓰기 → TCP 스트림 오염 → 소켓 파괴
    //   → 이후 DummyClient의 LEAVE_REQ가 GameServer에 전달되지 않음
    //
    // 변경 후: GatewaySession, ClientSession과 동일한 strand + deque 패턴 적용
    //   → 모든 Send() 호출이 strand_ 내부에서 직렬화되어 안전하게 전송됨
    // ==========================================
    boost::asio::io_context::strand strand_;
    std::deque<std::pair<std::shared_ptr<std::vector<char>>, size_t>> send_queue_;

    PacketHeader header_;
    std::vector<char> payload_buf_;
    std::string target_ip_;
    short target_port_;

public:
    GameConnection(boost::asio::io_context& io_context);
    void Connect(const std::string& ip, short port);
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void DoConnect();
    void ScheduleRetry();
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);

    // ★ [버그 픽스] 큐에서 패킷을 꺼내 실제로 전송하는 내부 함수
    void DoWrite();
};
