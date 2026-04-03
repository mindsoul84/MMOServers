#pragma once
#include <boost/asio.hpp>
#include <google/protobuf/message.h>

class PacketCrypto;

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

// ==========================================
//   패킷 암호화 지원 추가
//
// crypto 파라미터가 nullptr이 아니고 초기화된 상태이면
// Protobuf 페이로드를 AES-128-CBC로 암호화하여 전송합니다.
// crypto가 nullptr이면 기존과 동일하게 평문 전송합니다.
//
// 기본값 nullptr이므로 기존 호출부(LoginServer 통신 등)는 변경 불필요합니다.
// ==========================================
void SendPacket(boost::asio::ip::tcp::socket& socket, uint16_t pktId,
    const google::protobuf::Message& msg, PacketCrypto* crypto = nullptr);
