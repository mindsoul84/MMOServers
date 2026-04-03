#pragma once
#include <memory>
#include <cstdint>

class GatewaySession;

void Handle_GatewayGameMoveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize);
void Handle_GatewayGameLeaveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize);
void Handle_GatewayGameAttackReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t size);

//   채팅 AOI 처리: Gateway로부터 채팅을 받아 AOI 대상을 계산하여 반환
void Handle_GatewayGameChatReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize);
