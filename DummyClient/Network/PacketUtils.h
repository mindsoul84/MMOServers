#pragma once
#include <boost/asio.hpp>
#include <google/protobuf/message.h>

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

// 프로토버프 메시지를 직렬화하여 소켓으로 전송하는 헬퍼 함수
void SendPacket(boost::asio::ip::tcp::socket& socket, uint16_t pktId, const google::protobuf::Message& msg);