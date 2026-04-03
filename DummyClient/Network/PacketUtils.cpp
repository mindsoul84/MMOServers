#include "PacketUtils.h"
#include "../../Common/Network/PacketCrypto.h"
#include <vector>

// ==========================================
//   패킷 전송 시 암호화 지원
//
// crypto가 유효하고 초기화된 상태이면:
//   1. Protobuf 메시지를 직렬화
//   2. PacketCrypto::Encrypt()로 암호화 → [SeqNum][IV][CipherText]
//   3. 암호화된 데이터를 페이로드로 전송
//   PacketHeader.size에는 암호화 오버헤드가 포함됨
//
// crypto가 nullptr이거나 미초기화 상태이면 기존 평문 전송과 동일
// ==========================================
void SendPacket(boost::asio::ip::tcp::socket& socket, uint16_t pktId,
    const google::protobuf::Message& msg, PacketCrypto* crypto) {
    std::string payload;
    msg.SerializeToString(&payload);

    std::vector<char> final_payload;

    if (crypto && crypto->IsInitialized()) {
        auto result = crypto->Encrypt(payload.data(), static_cast<uint16_t>(payload.size()));
        if (result.success) {
            final_payload = std::move(result.data);
        }
        else {
            // 암호화 실패 시 평문 폴백 (개발 환경 안전장치)
            final_payload.assign(payload.begin(), payload.end());
        }
    }
    else {
        final_payload.assign(payload.begin(), payload.end());
    }

    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + final_payload.size());
    header.id = pktId;

    std::vector<char> send_buffer(header.size);
    memcpy(send_buffer.data(), &header, sizeof(PacketHeader));
    memcpy(send_buffer.data() + sizeof(PacketHeader), final_payload.data(), final_payload.size());

    boost::asio::write(socket, boost::asio::buffer(send_buffer));
}
