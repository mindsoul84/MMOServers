#pragma once
#include <memory>
#include <cstdint>

class WorldConnection;

void Handle_WorldGameMonsterBuff(std::shared_ptr<WorldConnection>& session, char* payload, uint16_t payloadSize);