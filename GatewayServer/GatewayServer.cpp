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
    // GatewayContext를 통해 디스패처 등록
    // ★ 기존: g_gateway_dispatcher.RegisterHandler(...)
    // ★ 수정: GatewayContext::Get().clientDispatcher.RegisterHandler(...)
    // ==========================================
    auto& ctx = GatewayContext::Get();

    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_CONNECT_REQ, Handle_GatewayConnectReq);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_CHAT_REQ,    Handle_ChatReq);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_MOVE_REQ,    Handle_MoveReq);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_ATTACK_REQ,  Handle_AttackReq);

    ctx.gameDispatcher.RegisterHandler(Protocol::PKT_GAME_GATEWAY_MOVE_RES,   Handle_MoveRes_FromGame);
    ctx.gameDispatcher.RegisterHandler(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, Handle_GameGatewayAttackRes);

    try {
        boost::asio::io_context io_context;

        // GameServer(9000)로 S2S 접속 (Context를 통해 저장)
        ctx.gameConnection = std::make_shared<GameConnection>(std::ref(io_context));
        ctx.gameConnection->Connect("127.0.0.1", 9000);

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