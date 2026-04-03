#include "GameHandlers.h"
#include "..\\GatewayServer\\GatewayServer.h"
#include "..\\Common\\Protocol\\protocol.pb.h"
#include "..\\GatewayServer\\Session\\ClientSession.h"
#include "..\\GatewayServer\\Network\\GameConnection.h"
#include "..\\Common\\Utils\\Logger.h"

#include <iostream>
#include <mutex>

void Handle_MoveRes_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize) {
    Protocol::GameGatewayMoveRes s2s_res;

    //   ParseFromArray 실패 시 로그 출력
    if (!s2s_res.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: GameGatewayMoveRes (payloadSize=" << payloadSize << ")");
        return;
    }

    Protocol::MoveRes client_res;
    client_res.set_account_id(s2s_res.account_id());
    client_res.set_x(s2s_res.x());
    client_res.set_y(s2s_res.y());
    client_res.set_z(s2s_res.z());
    client_res.set_yaw(s2s_res.yaw());

    auto& ctx = GatewayContext::Get();
    UTILITY::LockGuard lock(ctx.clientMutex);
    for (const std::string& target_id : s2s_res.target_account_ids()) {
        auto it = ctx.clientMap.find(target_id);
        if (it != ctx.clientMap.end() && it->second) {
            it->second->Send(Protocol::PKT_GATEWAY_CLIENT_MOVE_RES, client_res);
        }
    }
}

void Handle_GameGatewayAttackRes(std::shared_ptr<GameConnection>& session, char* payload, uint16_t size) {
    Protocol::GameGatewayAttackRes s2s_res;

    if (!s2s_res.ParseFromArray(payload, size)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: GameGatewayAttackRes (payloadSize=" << size << ")");
        return;
    }

    Protocol::AttackRes client_res;
    client_res.set_attacker_uid(s2s_res.attacker_uid());
    client_res.set_target_account_id(s2s_res.target_account_id());
    client_res.set_damage(s2s_res.damage());
    client_res.set_target_remain_hp(s2s_res.target_remain_hp());

    auto& ctx = GatewayContext::Get();
    UTILITY::LockGuard lock(ctx.clientMutex);
    for (int i = 0; i < s2s_res.target_account_ids_size(); ++i) {
        const std::string& target_id = s2s_res.target_account_ids(i);
        auto it = ctx.clientMap.find(target_id);
        if (it != ctx.clientMap.end() && it->second) {
            it->second->Send(Protocol::PKT_GATEWAY_CLIENT_ATTACK_RES, client_res);
        }
    }
}

// ==========================================
//   GameServer 경유 토큰 통지 수신
//
// 토큰 경로: WorldServer -> GameServer -> GatewayServer (이 핸들러)
// 수신한 토큰을 GatewayContext의 pendingTokens에 저장합니다.
// 이후 클라이언트가 GatewayConnectReq를 보내면 이 토큰으로 검증합니다.
// ==========================================
void Handle_TokenNotify_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize) {
    Protocol::TokenNotify notify;
    if (!notify.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: TokenNotify (payloadSize=" << payloadSize << ")");
        return;
    }

    auto& ctx = GatewayContext::Get();
    ctx.StorePendingToken(notify.account_id(), notify.session_token(), notify.expire_time_ms());

    LOG_INFO("Gateway", "토큰 수신 완료 (유저: " << notify.account_id() << ")");
}

// ==========================================
//   GameServer로부터 채팅 AOI 응답 수신
//
// 변경 전: 채팅을 clientMap 전체에 브로드캐스트 (O(N))
// 변경 후: GameServer가 AOI 내 유저 목록을 계산하여 반환
//          해당 유저들에게만 채팅을 전달 (O(K), K = AOI 내 유저 수)
// ==========================================
void Handle_ChatRes_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize) {
    Protocol::GameGatewayChatRes s2s_res;
    if (!s2s_res.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: GameGatewayChatRes (payloadSize=" << payloadSize << ")");
        return;
    }

    Protocol::ChatRes client_res;
    client_res.set_account_id(s2s_res.account_id());
    client_res.set_msg(s2s_res.msg());

    auto& ctx = GatewayContext::Get();
    UTILITY::LockGuard lock(ctx.clientMutex);

    for (int i = 0; i < s2s_res.target_account_ids_size(); ++i) {
        const std::string& target_id = s2s_res.target_account_ids(i);
        auto it = ctx.clientMap.find(target_id);
        if (it != ctx.clientMap.end() && it->second) {
            it->second->Send(Protocol::PKT_GATEWAY_CLIENT_CHAT_RES, client_res);
        }
    }
}
