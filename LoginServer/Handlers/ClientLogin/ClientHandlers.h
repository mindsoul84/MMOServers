#pragma once
#include <memory>
#include <cstdint>

class Session;

void Handle_LoginReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize);
void Handle_Heartbeat(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize);
void Handle_WorldSelectReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize);