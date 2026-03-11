#pragma once
#include <memory>
#include <cstdint>

class WorldConnection;

void Handle_S2SWorldSelectRes(std::shared_ptr<WorldConnection>& world_conn, char* payload, uint16_t payloadSize);