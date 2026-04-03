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
#include "..\..\Common\Network\PacketAssembler.h"
#include "..\..\Common\Network\PacketCrypto.h"
#include "..\..\Common\Define\SecurityConstants.h"

struct SendBuffer;

// ==========================================
// [패킷 파이프라인 강화] Rate Limiter + Parse Violation Tracker + AES 암호화
//
//   패킷 암호화 통합 (PacketCrypto)
//   변경 전: PacketCrypto 클래스가 존재하지만 실제 통신 경로에 미연동
//     -> TCP 평문 통신으로 패킷 스니핑에 완전 노출
//
//   변경 후: ClientSession의 Send/ReadPayload에 PacketCrypto를 통합
//     -> GatewayConnectReq/Res 핸드셰이크 완료 후 암호화 활성화
//     -> 이후 모든 패킷의 페이로드가 AES-128-CBC로 암호화됨
//     -> 시퀀스 번호로 리플레이 공격 방지
//     -> S2S 통신은 내부망이므로 암호화하지 않음 (CryptoConstants::ENCRYPT_S2S = false)
//
// [암호화 패킷 구조]
//   핸드셰이크 전: [PacketHeader(4B)][Protobuf Payload(N)]
//   핸드셰이크 후: [PacketHeader(4B)][SeqNum(4B)][IV(16B)][EncryptedPayload(N+pad)]
//   PacketHeader.size는 항상 전체 크기를 포함하므로 수신 측에서 정확한 크기를 알 수 있음
// ==========================================
class ClientSession : public std::enable_shared_from_this<ClientSession> {
private:
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context::strand strand_;
    std::deque<std::pair<std::shared_ptr<SendBuffer>, size_t>> send_queue_;

    PacketHeader header_;

    PacketAssembler assembler_;

    std::string account_id_ = "";

    // [패킷 파이프라인] 세션별 Rate Limiter
    PacketRateLimiter rate_limiter_;
    int rate_violation_count_ = 0;

    // ParseFromArray 실패 추적기
    ParseViolationTracker parse_tracker_;

    //   세션별 패킷 암호화 (AES-128-CBC + 시퀀스 번호)
    PacketCrypto crypto_;
    bool crypto_enabled_ = false;

public:
    ClientSession(boost::asio::ip::tcp::socket socket) noexcept;
    void start();
    void SetAccountId(const std::string& id);
    const std::string& GetAccountId() const;
    void Send(uint16_t pktId, const google::protobuf::Message& msg);
    void OnDisconnected();

    bool OnParseViolation();
    void OnParseSuccess();

    //   핸드셰이크 완료 후 암호화 활성화
    // Handle_GatewayConnectReq에서 토큰 검증 성공 시 호출
    void EnableEncryption();

private:
    void ReadHeader();
    void ReadPayload(uint16_t payload_size);
    void DoWrite();
};
