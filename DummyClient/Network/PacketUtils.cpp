#include "PacketUtils.h"
#include <vector>

void SendPacket(boost::asio::ip::tcp::socket& socket, uint16_t pktId, const google::protobuf::Message& msg) {
    std::string payload;
    msg.SerializeToString(&payload);

    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
    header.id = pktId;

    std::vector<char> send_buffer(header.size);
    memcpy(send_buffer.data(), &header, sizeof(PacketHeader));
    memcpy(send_buffer.data() + sizeof(PacketHeader), payload.data(), payload.size());

    boost::asio::write(socket, boost::asio::buffer(send_buffer));
}