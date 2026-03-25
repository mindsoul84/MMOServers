#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <vector>
#include <deque>   // [BUG FIX] send queue
#include <utility> // [BUG FIX] std::pair
#include <google/protobuf/message.h>
#include "..\GatewayServer.h"

struct SendBuffer; // forward declaration

// ==========================================
// [BUG FIX] strand + send_queue 추가
//
// 변경 전: async_write를 strand 없이 직접 호출
//   -> 멀티스레드 환경에서 동일 소켓에 동시 쓰기 시 TCP 스트림 interleave (패킷 깨짐)
// 변경 후: GatewaySession과 동일한 strand + deque 패턴 적용
//   -> 모든 Send()가 strand_ 내부에서 직렬화되어 안전하게 전송됨
// ==========================================
class ClientSession : public std::enable_shared_from_this<ClientSession> {
private:
    boost::asio::ip::tcp::socket socket_;

    // [BUG FIX] 이 세션 전용 strand (동시 async_write 방지)
    boost::asio::io_context::strand strand_;

    // [BUG FIX] 패킷 전송 대기열 (버퍼 포인터 + 실제 전송 크기)
    std::deque<std::pair<std::shared_ptr<SendBuffer>, size_t>> send_queue_;

    PacketHeader header_;
    std::vector<char> payload_buf_;
    std::string account_id_ = "";

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

    // [BUG FIX] 큐에서 패킷을 꺼내 실제로 전송하는 내부 함수
    void DoWrite();
};
