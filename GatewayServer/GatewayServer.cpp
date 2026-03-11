#include "GatewayServer.h"
#include "Session/ClientSession.h"
#include "Network/GameConnection.h"
#include "Handlers/ClientGateway/ClientHandlers.h"
#include "Handlers/GameGateway/GameHandlers.h"

#include <iostream>
#include <windows.h>
#include <thread>

using boost::asio::ip::tcp;

// ==========================================
// ★ 전역 메모리 할당
// ==========================================
PacketDispatcher<GameConnection> g_game_dispatcher;
PacketDispatcher<ClientSession> g_gateway_dispatcher;

std::shared_ptr<GameConnection> g_gameConnection; // S2S (GameServer 연결용)

std::unordered_map<std::string, std::shared_ptr<ClientSession>> g_clientMap; // 접속 클라이언트 세션 맵
std::mutex g_clientMutex;


// ==========================================
// GatewayServer 수신 대기열 및 Main
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
    g_gateway_dispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_ATTACK_REQ, Handle_AttackReq);

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