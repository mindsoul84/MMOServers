#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>

#include "..\Common\Protocol\protocol.pb.h"
#include "PacketDispatcher.h"

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

class GameConnection;
class ClientSession;

// ==========================================
// ★ 전역 변수 extern 선언
// ==========================================
extern PacketDispatcher<GameConnection> g_game_dispatcher;
extern PacketDispatcher<ClientSession> g_gateway_dispatcher;

extern std::shared_ptr<GameConnection> g_gameConnection;

extern std::unordered_map<std::string, std::shared_ptr<ClientSession>> g_clientMap;
extern std::mutex g_clientMutex;