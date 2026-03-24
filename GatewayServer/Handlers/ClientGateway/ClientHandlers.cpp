#include "ClientHandlers.h"
#include "..\\GatewayServer\\GatewayServer.h"
#include "..\\Common\\Protocol\\protocol.pb.h"
#include "..\\GatewayServer\\Session\\ClientSession.h"
#include "..\\GatewayServer\\Network\\GameConnection.h"

#include <iostream>
#include <mutex>

void Handle_GatewayConnectReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::GatewayConnectReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        session->SetAccountId(req.account_id());

        auto& ctx = GatewayContext::Get();
        {
            std::lock_guard<std::mutex> lock(ctx.clientMutex);
            ctx.clientMap[req.account_id()] = session;
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

        auto& ctx = GatewayContext::Get();
        std::lock_guard<std::mutex> lock(ctx.clientMutex);
        for (auto& pair : ctx.clientMap) {
            if (pair.second) pair.second->Send(Protocol::PKT_GATEWAY_CLIENT_CHAT_RES, res);
        }
    }
}

void Handle_MoveReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::MoveReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        auto& ctx = GatewayContext::Get();
        if (ctx.gameConnection) {
            Protocol::GatewayGameMoveReq s2s_req;
            s2s_req.set_account_id(session->GetAccountId());
            s2s_req.set_x(req.x());
            s2s_req.set_y(req.y());
            s2s_req.set_z(req.z());
            s2s_req.set_yaw(req.yaw());
            ctx.gameConnection->Send(Protocol::PKT_GATEWAY_GAME_MOVE_REQ, s2s_req);
        }
    }
}

void Handle_AttackReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::AttackReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        auto& ctx = GatewayContext::Get();
        if (ctx.gameConnection) {
            Protocol::GatewayGameAttackReq s2s_req;
            s2s_req.set_account_id(session->GetAccountId());
            ctx.gameConnection->Send(Protocol::PKT_GATEWAY_GAME_ATTACK_REQ, s2s_req);
        }
    }
}
