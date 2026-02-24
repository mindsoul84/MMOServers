#include <iostream>
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <windows.h>
#include <thread>

#include "protocol.pb.h"
#include "PacketDispatcher.h"

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

using boost::asio::ip::tcp;

// ==========================================
// S2S(Server-to-Server) 통신 세션
// ==========================================
class ServerSession;
PacketDispatcher<ServerSession> g_s2s_dispatcher;

class ServerSession : public std::enable_shared_from_this<ServerSession> {
private:
    tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    ServerSession(tcp::socket socket) noexcept : socket_(std::move(socket)) {}

    void start() {
        ReadHeader();
    }

    void Send(uint16_t pktId, const google::protobuf::Message& msg) {
        std::string payload;
        msg.SerializeToString(&payload);

        PacketHeader header;
        header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
        header.id = pktId;

        auto send_buf = std::make_shared<std::vector<char>>(header.size);
        memcpy(send_buf->data(), &header, sizeof(PacketHeader));
        memcpy(send_buf->data() + sizeof(PacketHeader), payload.data(), payload.size());

        auto self(shared_from_this());
        boost::asio::async_write(socket_, boost::asio::buffer(*send_buf),
            [this, self, send_buf](boost::system::error_code ec, std::size_t /*length*/) {
                if (ec) std::cerr << "[WorldServer] S2S 패킷 전송 실패\n";
            });
    }

private:
    void ReadHeader() {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    if (header_.size < sizeof(PacketHeader) || header_.size > 4096) return;
                    uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));

                    if (payload_size == 0) {
                        auto session_ptr = self;
                        g_s2s_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                        ReadHeader();
                    }
                    else {
                        payload_buf_.resize(payload_size);
                        ReadPayload(payload_size);
                    }
                }
                else {
                    std::cout << "[WorldServer] 연동된 서버(LoginServer 등)와의 연결이 해제되었습니다.\n";
                }
            });
    }

    void ReadPayload(uint16_t payload_size) {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
            [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    auto session_ptr = self;
                    g_s2s_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                    ReadHeader();
                }
            });
    }
};

// ==========================================
// 핸들러: LoginServer가 보낸 월드 선택 요청(1010) 처리
// ==========================================
void Handle_WorldLoginSelectReq(std::shared_ptr<ServerSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::LoginWorldSelectReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        std::cout << "[WorldServer] LoginServer로부터 유저(" << req.account_id() << ")의 월드 " << req.world_id() << "번 입장 요청 수신.\n";

        // 응답 패킷 세팅
        Protocol::WorldLoginSelectRes res;
        res.set_account_id(req.account_id());
        res.set_success(true);
        res.set_gateway_ip("127.0.0.1"); // 해당 월드를 담당하는 Gateway IP
        res.set_gateway_port(8888);      // Gateway Port

        // 월드서버가 직접 고유한 접속 토큰 발급
        std::string new_token = "WORLD_" + std::to_string(req.world_id()) + "_TOKEN_" + req.account_id();
        res.set_session_token(new_token);

        session->Send(Protocol::PKT_WORLD_LOGIN_SELECT_RES, res);
        std::cout << "[WorldServer] 유저(" << req.account_id() << ") 토큰 발급 및 LoginServer로 응답 완료.\n";
    }
}

// ==========================================
// WorldServer 대기열
// ==========================================
class WorldServer {
private:
    tcp::acceptor acceptor_;

public:
    WorldServer(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    // 로그 수정: 하위 서버가 아니라 동등한 피어 서버 접속으로 명시
                    std::cout << "[WorldServer] S2S 통신: LoginServer 접속 확인!\n";
                    std::make_shared<ServerSession>(std::move(socket))->start();
                }
                do_accept();
            });
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // 디스패처(Dispatcher)에 1010 패킷 핸들러 등록
    g_s2s_dispatcher.RegisterHandler(Protocol::PKT_LOGIN_WORLD_SELECT_REQ, Handle_WorldLoginSelectReq);

    try {
        boost::asio::io_context io_context;
        WorldServer server(io_context, 7000);
        std::cout << "[WorldServer] 월드 중앙 서버 가동 시작 (Port: 7000) Created by Jeong Shin Young\n";
        std::cout << "=================================================\n";

        io_context.run();
    }
    catch (std::exception& e) {
        std::cerr << "[Error] 월드 서버 예외 발생: " << e.what() << "\n";
    }
    return 0;
}