#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>

#include <deque>   // ★ [추가] 큐 사용
#include <utility> // ★ [추가] std::pair 사용

#include <google/protobuf/message.h>
#include "../GameServer.h"

struct SendBuffer; // 전방 선언

class GatewaySession : public std::enable_shared_from_this<GatewaySession> {
private:
    boost::asio::ip::tcp::socket socket_;

    // ★ [추가] 이 세션만을 위한 전용 1차선 도로 (동시 접근 방지)
    boost::asio::io_context::strand strand_;

    // ★ [추가] 패킷 전송 대기열 (버퍼 포인터와 실제 전송할 크기를 묶어서 보관)
    std::deque<std::pair<std::shared_ptr<SendBuffer>, size_t>> send_queue_;

    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    GatewaySession(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void Send(uint16_t pktId, const google::protobuf::Message& msg);

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);

    // ★ [추가] 큐에서 패킷을 꺼내 실제로 전송하는 내부 함수
    void DoWrite();

    // ★ [추가] 연결 끊김 시 메모리 회수를 전담할 중앙 처리 함수
    void OnDisconnected();
};