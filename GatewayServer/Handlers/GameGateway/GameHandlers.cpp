#include "GameHandlers.h"
#include "..\\GatewayServer\\GatewayServer.h"
#include "..\\Common\\Protocol\\protocol.pb.h"
#include "..\\GatewayServer\\Session\\ClientSession.h"
#include "..\\GatewayServer\\Network\\GameConnection.h"

#include <iostream>
#include <mutex>

void Handle_MoveRes_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize) {
    Protocol::GameGatewayMoveRes s2s_res;
    if (s2s_res.ParseFromArray(payload, payloadSize)) {
        Protocol::MoveRes client_res;
        client_res.set_account_id(s2s_res.account_id());
        client_res.set_x(s2s_res.x());
        client_res.set_y(s2s_res.y());
        client_res.set_z(s2s_res.z());
        client_res.set_yaw(s2s_res.yaw());

        auto& ctx = GatewayContext::Get();
        std::lock_guard<std::mutex> lock(ctx.clientMutex);
        for (const std::string& target_id : s2s_res.target_account_ids()) {
            auto it = ctx.clientMap.find(target_id);
            if (it != ctx.clientMap.end() && it->second) {
                it->second->Send(Protocol::PKT_GATEWAY_CLIENT_MOVE_RES, client_res);
            }
        }
    }
}

void Handle_GameGatewayAttackRes(std::shared_ptr<GameConnection>& session, char* payload, uint16_t size) {
    Protocol::GameGatewayAttackRes s2s_res;
    if (s2s_res.ParseFromArray(payload, size)) {
        Protocol::AttackRes client_res;
        client_res.set_attacker_uid(s2s_res.attacker_uid());
        client_res.set_target_account_id(s2s_res.target_account_id());
        client_res.set_damage(s2s_res.damage());
        client_res.set_target_remain_hp(s2s_res.target_remain_hp());

        auto& ctx = GatewayContext::Get();
        std::lock_guard<std::mutex> lock(ctx.clientMutex);
        for (int i = 0; i < s2s_res.target_account_ids_size(); ++i) {
            const std::string& target_id = s2s_res.target_account_ids(i);
            auto it = ctx.clientMap.find(target_id);
            if (it != ctx.clientMap.end() && it->second) {
                it->second->Send(Protocol::PKT_GATEWAY_CLIENT_ATTACK_RES, client_res);
            }
        }
    }
}
