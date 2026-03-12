#pragma once
#include <memory>
#include <cstdint>

class GatewaySession;

void Handle_GatewayGameMoveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize);
void Handle_GatewayGameLeaveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize);
void Handle_GatewayGameAttackReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t size);