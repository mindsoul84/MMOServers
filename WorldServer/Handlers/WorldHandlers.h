#pragma once
#include <memory>
#include <cstdint>

class ServerSession;

// LoginServer가 보낸 월드 선택 요청(1010) 처리
void Handle_WorldLoginSelectReq(std::shared_ptr<ServerSession>& session, char* payload, uint16_t payloadSize);
