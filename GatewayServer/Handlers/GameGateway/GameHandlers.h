#pragma once
#include <memory>
#include <cstdint>

class GameConnection;

void Handle_MoveRes_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize);
void Handle_GameGatewayAttackRes(std::shared_ptr<GameConnection>& session, char* payload, uint16_t size);