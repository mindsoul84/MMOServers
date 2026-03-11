#pragma once
#include <memory>
#include <cstdint>

class ClientSession;

void Handle_GatewayConnectReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize);
void Handle_ChatReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize);
void Handle_MoveReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize);
void Handle_AttackReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize);