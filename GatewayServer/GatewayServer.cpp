#include "GatewayServer.h"
#include "Session/ClientSession.h"
#include "Network/GameConnection.h"
#include "Handlers/ClientGateway/ClientHandlers.h"
#include "Handlers/GameGateway/GameHandlers.h"
#include "../Common/ConfigManager.h"
#include "../Common/MemoryPool.h"

#include <iostream>
#include <windows.h>
#include <thread>

using boost::asio::ip::tcp;

// ★ 정적 멤버 정의 (싱글톤 DI 오버라이드 포인터)
// s_test_instance_ 는 헤더에서 inline 으로 정의됩니다. (중복 정의 제거)

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

    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\GatewayServer_Unique_Mutex_Lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "🚨 [Error] GatewayServer가 이미 실행 중입니다. 창을 닫습니다.\n";
        CloseHandle(hMutex);
        return 1;
    }

    if (!ConfigManager::GetInstance().LoadConfig("config.json")) {
        std::cerr << "🚨 config 설정 파일 오류로 인해 GatewayServer 종료합니다.\n";
        system("pause");
        return -1;
    }

    // ★ [추가] 서버 역할에 맞는 메모리 풀 초기화 (GatewayServer는 대용량 서버)
    SendBufferPool::GetInstance().Initialize(PoolConfig::HEAVY_SERVER);

    auto& ctx = GatewayContext::Get();

    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_CONNECT_REQ, Handle_GatewayConnectReq);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_CHAT_REQ,    Handle_ChatReq);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_MOVE_REQ,    Handle_MoveReq);
    ctx.clientDispatcher.RegisterHandler(Protocol::PKT_CLIENT_GATEWAY_ATTACK_REQ,  Handle_AttackReq);

    ctx.gameDispatcher.RegisterHandler(Protocol::PKT_GAME_GATEWAY_MOVE_RES,   Handle_MoveRes_FromGame);
    ctx.gameDispatcher.RegisterHandler(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, Handle_GameGatewayAttackRes);

    try {
        boost::asio::io_context io_context;

        ctx.gameConnection = std::make_shared<GameConnection>(std::ref(io_context));
        short game_port = ConfigManager::GetInstance().GetGatewayGameConnPort();
        ctx.gameConnection->Connect("127.0.0.1", game_port);

        short gateway_port = ConfigManager::GetInstance().GetGatewayServerPort();
        GatewayServer server(io_context, gateway_port);
        std::cout << "[GatewayServer] 게임 게이트웨이 서버 가동 시작 (Port:" << gateway_port << ") Created by Jeong Shin Young\n";
        std::cout << "=================================================\n";

        unsigned int max_thread_count = ConfigManager::GetInstance().GetGatewayMaxThreadCount();
        if (max_thread_count == 0) max_thread_count = std::thread::hardware_concurrency();

        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < max_thread_count; ++i) {
            threads.emplace_back([&io_context]() { io_context.run(); });
        }
        for (auto& t : threads) { if (t.joinable()) t.join(); }
    }
    catch (std::exception& e) { std::cerr << "[Error] " << e.what() << "\n"; }
    return 0;
}
