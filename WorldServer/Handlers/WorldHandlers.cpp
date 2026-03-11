#include "WorldHandlers.h"
#include <iostream>
#include <string>

#include "..\Common\Protocol\protocol.pb.h"
#include "../WorldServer.h" // ★ 패킷 전송을 위해 공통 헤더 포함

void Handle_WorldLoginSelectReq(std::shared_ptr<ServerSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::LoginWorldSelectReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        std::cout << "[WorldServer] LoginServer로부터 유저(" << req.account_id() << ")의 월드 " << req.world_id() << "번 입장 요청 수신.\n";

        Protocol::WorldLoginSelectRes res;
        res.set_account_id(req.account_id());
        res.set_success(true);
        res.set_gateway_ip("127.0.0.1");
        res.set_gateway_port(8888);

        std::string new_token = "WORLD_" + std::to_string(req.world_id()) + "_TOKEN_" + req.account_id();
        res.set_session_token(new_token);

        session->Send(Protocol::PKT_WORLD_LOGIN_SELECT_RES, res);
        std::cout << "[WorldServer] 유저(" << req.account_id() << ") 토큰 발급 및 LoginServer로 응답 완료.\n";
    }
}