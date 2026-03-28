#pragma once
#include <memory>
#include <cstdint>

class GameConnection;

void Handle_MoveRes_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize);
void Handle_GameGatewayAttackRes(std::shared_ptr<GameConnection>& session, char* payload, uint16_t size);

// [추가] GameServer로부터 토큰 통지 수신
void Handle_TokenNotify_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize);

// [추가] GameServer로부터 채팅 AOI 응답 수신
void Handle_ChatRes_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize);
