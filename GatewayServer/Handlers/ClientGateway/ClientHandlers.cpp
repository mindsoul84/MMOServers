#include "ClientHandlers.h"
#include "..\GatewayServer\GatewayServer.h"
#include "..\Common\Protocol\protocol.pb.h"
#include "..\GatewayServer\Session\ClientSession.h"
#include "..\GatewayServer\Network\GameConnection.h"

#include <iostream>
#include <mutex>

// ==========================================
// 4. 인게임 패킷 핸들러 (Gateway 전용)
// ==========================================
void Handle_GatewayConnectReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::GatewayConnectReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        session->SetAccountId(req.account_id());

        {
            std::lock_guard<std::mutex> lock(g_clientMutex);
            g_clientMap[req.account_id()] = session;
        }

        Protocol::GatewayConnectRes res;
        res.set_success(true);
        session->Send(Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES, res);
        std::cout << "[Gateway] 유저(" << req.account_id() << ") 인게임 입장 승인 및 등록 완료!\n";
    }
}

void Handle_ChatReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::ChatReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        std::cout << "[Chat] " << session->GetAccountId() << " : " << req.msg() << "\n";

        Protocol::ChatRes res;
        res.set_account_id(session->GetAccountId());
        res.set_msg(req.msg());

        std::lock_guard<std::mutex> lock(g_clientMutex);
        for (auto& pair : g_clientMap) {
            auto client_session = pair.second;
            if (client_session) {
                client_session->Send(Protocol::PKT_GATEWAY_CLIENT_CHAT_RES, res);
            }
        }
    }
}

// [클라이언트 -> 게이트웨이] 유저가 움직였을 때
void Handle_MoveReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::MoveReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        // 내 ID를 붙여서 GameServer로 S2S 토스 (라우팅)
        if (g_gameConnection) {
            Protocol::GatewayGameMoveReq s2s_req;
            s2s_req.set_account_id(session->GetAccountId());
            s2s_req.set_x(req.x());
            s2s_req.set_y(req.y());
            s2s_req.set_z(req.z());
            s2s_req.set_yaw(req.yaw());

            g_gameConnection->Send(Protocol::PKT_GATEWAY_GAME_MOVE_REQ, s2s_req);
        }
    }
}

// [클라이언트 -> 게이트웨이] 유저가 'a' 키를 눌러 공격 패킷을 보냈을 때
void Handle_AttackReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::AttackReq req;
    if (req.ParseFromArray(payload, payloadSize)) {

        // GameServer로 S2S 토스 (내 Account ID를 담아서 쏩니다)
        if (g_gameConnection) {
            Protocol::GatewayGameAttackReq s2s_req;
            s2s_req.set_account_id(session->GetAccountId());

            // 만약 클라이언트가 타겟 몬스터 ID를 안 보냈다면 0으로 보냅니다. (서버가 알아서 찾음)
            // s2s_req.set_target_uid(req.target_uid()); 

            g_gameConnection->Send(Protocol::PKT_GATEWAY_GAME_ATTACK_REQ, s2s_req);
        }
    }
}