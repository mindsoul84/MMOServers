#include <iostream>
#include <boost/asio.hpp>
#include <memory>
#include <atomic>
#include <windows.h>
#include <thread>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>

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
// 전방 선언 및 전역 변수
// ==========================================
class Session;
class WorldConnection;

std::atomic<int> g_connected_clients{ 0 };
std::unordered_set<std::string> g_loggedInUsers;
std::unordered_map<std::string, std::shared_ptr<Session>> g_sessionMap;
std::mutex g_loginMutex;

PacketDispatcher<Session> g_client_dispatcher;
PacketDispatcher<WorldConnection> g_world_dispatcher;
std::shared_ptr<WorldConnection> g_worldConnection; // 전역 S2S 커넥션 포인터


// ===========================================================
// 클래스 정의 (함수에서 쓰기 전에 반드시 먼저 정의되어야 함)
// ===========================================================

// [WorldConnection 클래스] LoginServer -> WorldServer 통신용
class WorldConnection : public std::enable_shared_from_this<WorldConnection> {
private:
    tcp::socket socket_;
    boost::asio::io_context& io_context_;
    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    WorldConnection(boost::asio::io_context& io_context)
        : socket_(io_context), io_context_(io_context) {
    }

    void Connect(const std::string& ip, short port) {
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(ip, std::to_string(port));
        boost::asio::connect(socket_, endpoints);
        std::cout << "[LoginServer] 🌐 WorldServer(S2S)에 성공적으로 연결되었습니다!\n";
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
            [this, self, send_buf](boost::system::error_code ec, std::size_t) {
                if (ec) std::cerr << "[LoginServer] WorldServer로 패킷 전송 실패\n";
            });
    }

private:
    void ReadHeader() {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                    if (payload_size == 0) {
                        auto session_ptr = self;
                        g_world_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                        ReadHeader();
                    }
                    else {
                        payload_buf_.resize(payload_size);
                        ReadPayload(payload_size);
                    }
                }
                else {
                    std::cerr << "[LoginServer] 🚨 WorldServer와의 연결이 끊어졌습니다!\n";
                }
            });
    }

    void ReadPayload(uint16_t payload_size) {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
            [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    auto session_ptr = self;
                    g_world_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                    ReadHeader();
                }
            });
    }
};


// [Session 클래스] DummyClient -> LoginServer 통신용
class Session : public std::enable_shared_from_this<Session> {
private:
    tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;
    std::string logged_in_id_ = "";

public:
    Session(tcp::socket socket) noexcept : socket_(std::move(socket)) {}

    void start() { ReadHeader(); }
    void SetLoggedInId(const std::string& id) { logged_in_id_ = id; }
    const std::string& GetLoggedInId() const { return logged_in_id_; }

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
            [this, self, send_buf](boost::system::error_code ec, std::size_t) {});
    }

private:
    void OnDisconnected() {
        int current_count = --g_connected_clients;
        std::cout << "[LoginServer] 유저 접속 종료 (접속자: " << current_count << "명)\n";
        if (!logged_in_id_.empty()) {
            std::lock_guard<std::mutex> lock(g_loginMutex);
            g_loggedInUsers.erase(logged_in_id_);
            g_sessionMap.erase(logged_in_id_); // 맵에서도 삭제
        }
    }

    void ReadHeader() {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    if (header_.size < sizeof(PacketHeader) || header_.size > 4096) return;
                    uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));
                    if (payload_size == 0) {
                        auto session_ptr = self;
                        g_client_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                        ReadHeader();
                    }
                    else {
                        payload_buf_.resize(payload_size);
                        ReadPayload(payload_size);
                    }
                }
                else OnDisconnected();
            });
    }

    void ReadPayload(uint16_t payload_size) {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
            [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    auto session_ptr = self;
                    g_client_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                    ReadHeader();
                }
                else OnDisconnected();
            });
    }
};


// ==================================================================
// 3. 패킷 핸들러 (클래스가 모두 정의된 후 작성해야 정상 호출 가능)
// ==================================================================
void Handle_LoginReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::LoginReq login_req;
    if (login_req.ParseFromArray(payload, payloadSize)) {
        std::string req_id = login_req.id();
        Protocol::LoginRes login_res;
        {
            std::lock_guard<std::mutex> lock(g_loginMutex);
            if (g_loggedInUsers.find(req_id) != g_loggedInUsers.end()) {
                login_res.set_success(false);
                std::cout << "[로그인 거부] 이미 접속 중: " << req_id << "\n";
            }
            else {
                g_loggedInUsers.insert(req_id);
                g_sessionMap[req_id] = session; // 맵에 세션 저장
                session->SetLoggedInId(req_id);
                login_res.set_success(true);
                std::cout << "[로그인 승인] ID: " << req_id << " 접속 완료.\n";
            }
        }
        session->Send(Protocol::PKT_LOGIN_CLIENT_LOGIN_RES, login_res);
    }
}

void Handle_Heartbeat(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::Heartbeat hb;
    if (hb.ParseFromArray(payload, payloadSize)) {
        std::cout << "[패킷수신] PKT_HEARTBEAT - 클라이언트(" << session->GetLoggedInId() << ") 생존 확인!\n";
    }
}

void Handle_WorldSelectReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::WorldSelectReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        std::cout << "[LoginServer] 유저(" << session->GetLoggedInId() << ")가 월드 " << req.world_id() << "번 선택.\n";

        if (g_worldConnection) {
            Protocol::LoginWorldSelectReq s2s_req;
            s2s_req.set_account_id(session->GetLoggedInId());
            s2s_req.set_world_id(req.world_id());
            g_worldConnection->Send(Protocol::PKT_LOGIN_WORLD_SELECT_REQ, s2s_req);
        }
    }
}

void Handle_S2SWorldSelectRes(std::shared_ptr<WorldConnection>& world_conn, char* payload, uint16_t payloadSize) {
    Protocol::WorldLoginSelectRes res;
    if (res.ParseFromArray(payload, payloadSize)) {
        std::lock_guard<std::mutex> lock(g_loginMutex);
        auto it = g_sessionMap.find(res.account_id());
        if (it != g_sessionMap.end()) {
            std::shared_ptr<Session> client_session = it->second;

            Protocol::WorldSelectRes client_res;
            client_res.set_success(res.success());
            client_res.set_gateway_ip(res.gateway_ip());
            client_res.set_gateway_port(res.gateway_port());
            client_res.set_session_token(res.session_token());

            client_session->Send(Protocol::PKT_LOGIN_CLIENT_WORLD_SELECT_RES, client_res);
            std::cout << "[LoginServer] WorldServer의 응답(토큰)을 유저(" << res.account_id() << ")에게 릴레이 완료.\n";
        }
    }
}


// ==========================================
// 4. Server 대기열 및 Main
// ==========================================
class Server {
private:
    tcp::acceptor acceptor_;
public:
    Server(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }
private:
    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                g_connected_clients++;
                std::make_shared<Session>(std::move(socket))->start();
            }
            do_accept();
            });
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    g_client_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_LOGIN_LOGIN_REQ, Handle_LoginReq);
    g_client_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_SERVER_HEARTBEAT, Handle_Heartbeat);
    g_client_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_LOGIN_WORLD_SELECT_REQ, Handle_WorldSelectReq);
    g_world_dispatcher.RegisterHandler(Protocol::PKT_WORLD_LOGIN_SELECT_RES, Handle_S2SWorldSelectRes);

    try {
        boost::asio::io_context io_context;

        // ★ 에러 수정: std::ref()를 사용하여 io_context를 원본 참조로 완벽하게 넘겨줍니다.
        g_worldConnection = std::make_shared<WorldConnection>(std::ref(io_context));
        g_worldConnection->Connect("127.0.0.1", 7000);

        Server server(io_context, 7777);
        std::cout << "[LoginServer] 로그인 서버 가동 시작 (Port: 7777) Created by Jeong Shin Young\n";
        std::cout << "=================================================\n";

        unsigned int thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }
        for (auto& t : threads) { if (t.joinable()) t.join(); }
    }
    catch (std::exception& e) {
        std::cerr << "[Error] 서버 예외 발생: " << e.what() << "\n";
    }
    return 0;
}