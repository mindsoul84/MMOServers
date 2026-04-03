#include "WorldHandlers.h"
#include "../../Session/Session.h"
#include "../LoginServer/LoginServer.h"
#include "..\\Common\\Protocol\\protocol.pb.h"
#include <iostream>

void Handle_S2SWorldSelectRes(std::shared_ptr<WorldConnection>& world_conn, char* payload, uint16_t payloadSize) {
    Protocol::WorldLoginSelectRes res;

    //   ParseFromArray 실패 시 로그 출력
    if (!res.ParseFromArray(payload, payloadSize)) {
        std::cerr << "[LoginServer] ParseFromArray 실패: WorldLoginSelectRes (payloadSize=" << payloadSize << ")\n";
        return;
    }

    UTILITY::LockGuard lock(g_loginMutex);
    auto it = g_sessionMap.find(res.account_id());
    if (it != g_sessionMap.end()) {
        std::shared_ptr<Session> client_session = it->second;

        Protocol::WorldSelectRes client_res;
        client_res.set_success(res.success());
        client_res.set_gateway_ip(res.gateway_ip());
        client_res.set_gateway_port(res.gateway_port());
        client_res.set_session_token(res.session_token());

        client_session->Send(Protocol::PKT_LOGIN_CLIENT_WORLD_SELECT_RES, client_res);
        std::cout << "[LoginServer] WorldServer의 응답(토큰)을 유저(" << res.account_id() << ")에게 릴레이 완료.\n";
    }
    else {
        std::cerr << "[LoginServer] WorldSelectRes 처리 실패: 세션 맵에서 유저(" << res.account_id() << ")를 찾을 수 없음\n";
    }
}
