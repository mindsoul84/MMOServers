#pragma once
#include <memory>
#include <cstdint>

class WorldConnection;

void Handle_WorldGameMonsterBuff(std::shared_ptr<WorldConnection>& session, char* payload, uint16_t payloadSize);

// [추가] WorldServer로부터 토큰 통지를 수신하여 GatewayServer로 중계
void Handle_WorldGameTokenNotify(std::shared_ptr<WorldConnection>& session, char* payload, uint16_t payloadSize);
