#include <iostream>
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <windows.h>
#include <thread>
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

// ===============================================
// 1. 전방 선언 및 전역 변수 (디스패처, 세션 관리)
// ===============================================
class GameConnection;
class ClientSession;

PacketDispatcher<GameConnection> g_game_dispatcher;
PacketDispatcher<ClientSession> g_gateway_dispatcher;

std::shared_ptr<GameConnection> g_gameConnection; // S2S (GameServer 연결용)

std::unordered_map<std::string, std::shared_ptr<ClientSession>> g_clientMap; // 접속 클라이언트 세션 맵
std::mutex g_clientMutex;


// ====================================================
// 2. GameConnection (Gateway -> GameServer S2S 접속용)
// ====================================================
class GameConnection : public std::enable_shared_from_this<GameConnection> {
private:
    tcp::socket socket_;
    boost::asio::io_context& io_context_;
    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    GameConnection(boost::asio::io_context& io_context)
        : socket_(io_context), io_context_(io_context) {
    }

    void Connect(const std::string& ip, short port) {
        try {
            tcp::resolver resolver(io_context_);
            auto endpoints = resolver.resolve(ip, std::to_string(port));
            boost::asio::connect(socket_, endpoints);
            std::cout << "[Gateway] 🕹️ GameServer(S2S) 9000번 포트에 성공적으로 연결되었습니다!\n";
            ReadHeader();
        }
        catch (std::exception&) {
            // 강제 종료(Crash)를 막고 친절하게 알려줍니다.
            std::cerr << "🚨 [치명적 에러] GameServer(9000 포트)가 꺼져 있습니다! GameServer를 먼저 켜주세요!\n";            
            exit(1); // 안전하게 프로그램 종료
        }
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
                if (ec) std::cerr << "[Gateway] GameServer로 S2S 패킷 전송 실패\n";
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
                        // self의 복사본(session_ptr)을 만들어 넘깁니다.
                        auto session_ptr = self;
                        g_game_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                        ReadHeader();
                    }
                    else {
                        payload_buf_.resize(payload_size);
                        ReadPayload(payload_size);
                    }
                }
                else std::cerr << "[Gateway] 🚨 GameServer와의 연결이 끊어졌습니다!\n";
            });
    }

    void ReadPayload(uint16_t payload_size) {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
            [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    // 여기도 마찬가지로 복사본을 만들어 넘깁니다.
                    auto session_ptr = self;
                    g_game_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                    ReadHeader();
                }
            });
    }
};


// ================================================
// 3. ClientSession (DummyClient -> Gateway 접속용)
// ================================================
class ClientSession : public std::enable_shared_from_this<ClientSession> {
private:
    tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;
    std::string account_id_ = "";

public:
    ClientSession(tcp::socket socket) noexcept : socket_(std::move(socket)) {}
    void start() { ReadHeader(); }
    void SetAccountId(const std::string& id) { account_id_ = id; }
    const std::string& GetAccountId() const { return account_id_; }

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

    void OnDisconnected() {
        if (!account_id_.empty()) {
            // 맵에서 지우기 전에 GameServer로 "이 유저 나갔다"고 알려줍니다.
            if (g_gameConnection) {
                Protocol::GatewayGameLeaveReq leave_req;
                leave_req.set_account_id(account_id_);
                g_gameConnection->Send(Protocol::PKT_GATEWAY_GAME_LEAVE_REQ, leave_req);
            }

            // Gateway의 세션 맵에서 삭제
            std::lock_guard<std::mutex> lock(g_clientMutex);
            g_clientMap.erase(account_id_);
            std::cout << "[Gateway] 유저 접속 종료 및 맵에서 삭제됨: " << account_id_ << "\n";
            account_id_ = "";
        }
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
                        g_gateway_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
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
                    g_gateway_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                    ReadHeader();
                }
                else OnDisconnected();
            });
    }
};


// ==========================================
// 4. 인게임 패킷 핸들러 (Gateway 전용)
// ==========================================
void Handle_GatewayConnectReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::GatewayConnectReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        session->SetAccountId(req.account_id());

        {
            std::lock_guard<std::mutex> lock(g_clientMutex);
            g_clientMap[req.account_id()] = session;
        }

        Protocol::GatewayConnectRes res;
        res.set_success(true);
        session->Send(Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES, res);
        std::cout << "[Gateway] 유저(" << req.account_id() << ") 인게임 입장 승인 및 등록 완료!\n";
    }
}

void Handle_ChatReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::ChatReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        std::cout << "[Chat] " << session->GetAccountId() << " : " << req.msg() << "\n";

        Protocol::ChatRes res;
        res.set_account_id(session->GetAccountId());
        res.set_msg(req.msg());

        std::lock_guard<std::mutex> lock(g_clientMutex);
        for (auto& pair : g_clientMap) {
            auto client_session = pair.second;
            if (client_session) {
                client_session->Send(Protocol::PKT_GATEWAY_CLIENT_CHAT_RES, res);
            }
        }
    }
}

// [클라이언트 -> 게이트웨이] 유저가 움직였을 때
void Handle_MoveReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::MoveReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        // 내 ID를 붙여서 GameServer로 S2S 토스 (라우팅)
        if (g_gameConnection) {
            Protocol::GatewayGameMoveReq s2s_req;
            s2s_req.set_account_id(session->GetAccountId());
            s2s_req.set_x(req.x());
            s2s_req.set_y(req.y());
            s2s_req.set_z(req.z());
            s2s_req.set_yaw(req.yaw());

            g_gameConnection->Send(Protocol::PKT_GATEWAY_GAME_MOVE_REQ, s2s_req);
        }
    }
}

// [게임서버 -> 게이트웨이] S2S 이동 지시 패킷 수신
void Handle_MoveRes_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize) {
    Protocol::GameGatewayMoveRes s2s_res;
    if (s2s_res.ParseFromArray(payload, payloadSize)) {

        // 클라이언트들이 받을 실제 MoveRes 패킷 세팅
        Protocol::MoveRes client_res;
        client_res.set_account_id(s2s_res.account_id());
        client_res.set_x(s2s_res.x());
        client_res.set_y(s2s_res.y());
        client_res.set_z(s2s_res.z());
        client_res.set_yaw(s2s_res.yaw());

        std::lock_guard<std::mutex> lock(g_clientMutex);

        // ★ 핵심: 전체 맵(g_clientMap)을 무조건 도는 것이 아니라, 
        // GameServer가 계산해서 알려준 타겟 명단(AOI)만 쏙쏙 뽑아서 전송합니다!
        for (const std::string& target_id : s2s_res.target_account_ids()) {
            auto it = g_clientMap.find(target_id);
            if (it != g_clientMap.end()) {
                auto client_session = it->second;
                if (client_session) {
                    client_session->Send(Protocol::PKT_GATEWAY_CLIENT_MOVE_RES, client_res);
                }
            }
        }
    }
}

// ==========================================
// [추가] GameServer -> Gateway -> Client 전투(피격) 브로드캐스트 릴레이
// ==========================================
void Handle_GameGatewayAttackRes(std::shared_ptr<GameConnection>& session, char* payload, uint16_t size) {
    Protocol::GameGatewayAttackRes s2s_res;
    if (s2s_res.ParseFromArray(payload, size)) {

        // 1. 클라이언트가 읽을 수 있는 AttackRes 패킷으로 변환 (포장지 교체)
        Protocol::AttackRes client_res;
        client_res.set_attacker_uid(s2s_res.attacker_uid());
        client_res.set_target_account_id(s2s_res.target_account_id());
        client_res.set_damage(s2s_res.damage());
        client_res.set_target_remain_hp(s2s_res.target_remain_hp());

        // 2. 이펙트/데미지를 봐야 하는 주변 유저(AOI)들에게 각각 전송
        std::lock_guard<std::mutex> lock(g_clientMutex);
        for (int i = 0; i < s2s_res.target_account_ids_size(); ++i) {
            const std::string& target_id = s2s_res.target_account_ids(i);

            // 현재 게이트웨이 세션 맵에 접속해 있는 유저라면 패킷 쏘기!
            auto it = g_clientMap.find(target_id);
            if (it != g_clientMap.end() && it->second) {
                // PKT_GATEWAY_CLIENT_ATTACK_RES (27번) 으로 클라이언트에게 전송
                it->second->Send(Protocol::PKT_GATEWAY_CLIENT_ATTACK_RES, client_res);
            }
        }
    }
}

// ==========================================
// 5. GatewayServer 수신 대기열 및 Main
// ==========================================
class GatewayServer {
    tcp::acceptor acceptor_;
public:
    GatewayServer(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }
private:
    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) std::make_shared<ClientSession>(std::move(socket))->start();
            do_accept();
            });
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);
    // ==========================================
    // [클라이언트 -> 게이트웨이] 패킷 핸들러 등록
    // ==========================================
    g_gateway_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_CONNECT_REQ, Handle_GatewayConnectReq);
    g_gateway_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_CHAT_REQ, Handle_ChatReq);
    g_gateway_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_MOVE_REQ, Handle_MoveReq);

    // ==============================================
    // [게임서버 -> 게이트웨이(S2S)] 패킷 핸들러 등록
    // ==============================================
    // 주의: PKT_MOVE_RES(25)가 아니라 PKT_S2S_MOVE_RES(1025)를 등록해야 합니다.
    g_game_dispatcher.RegisterHandler(Protocol::PKT_GAME_GATEWAY_MOVE_RES, Handle_MoveRes_FromGame);
    g_game_dispatcher.RegisterHandler(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, Handle_GameGatewayAttackRes);

    try {
        boost::asio::io_context io_context;

        // ★ 1. 유저를 받기 전에 GameServer(9000번)로 먼저 S2S 접속 시도
        g_gameConnection = std::make_shared<GameConnection>(std::ref(io_context));
        g_gameConnection->Connect("127.0.0.1", 9000);

        // ★ 2. 클라이언트 대기열(8888번) 오픈
        GatewayServer server(io_context, 8888);
        std::cout << "[GatewayServer] 게임 게이트웨이 서버 가동 시작 (Port: 8888) Created by Jeong Shin Young\n";
        std::cout << "=================================================\n";

        unsigned int thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }
        for (auto& t : threads) { if (t.joinable()) t.join(); }
    }
    catch (std::exception& e) { std::cerr << "[Error] " << e.what() << "\n"; }
    return 0;
}