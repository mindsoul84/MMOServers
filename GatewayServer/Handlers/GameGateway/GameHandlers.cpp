#include "GameHandlers.h"
#include "..\GatewayServer\GatewayServer.h"
#include "..\Common\Protocol\protocol.pb.h"
#include "..\GatewayServer\Session\ClientSession.h"
#include "..\GatewayServer\Network\GameConnection.h"

#include <iostream>
#include <mutex>

// [게임서버 -> 게이트웨이] S2S 이동 지시 패킷 수신
void Handle_MoveRes_FromGame(std::shared_ptr<GameConnection>& conn, char* payload, uint16_t payloadSize) {
    Protocol::GameGatewayMoveRes s2s_res;
    if (s2s_res.ParseFromArray(payload, payloadSize)) {

        // 클라이언트들이 받을 실제 MoveRes 패킷 세팅
        Protocol::MoveRes client_res;
        client_res.set_account_id(s2s_res.account_id());
        client_res.set_x(s2s_res.x());
        client_res.set_y(s2s_res.y());
        client_res.set_z(s2s_res.z());
        client_res.set_yaw(s2s_res.yaw());

        std::lock_guard<std::mutex> lock(g_clientMutex);

        // ★ 핵심: 전체 맵(g_clientMap)을 무조건 도는 것이 아니라, 
        // GameServer가 계산해서 알려준 타겟 명단(AOI)만 쏙쏙 뽑아서 전송합니다!
        for (const std::string& target_id : s2s_res.target_account_ids()) {
            auto it = g_clientMap.find(target_id);
            if (it != g_clientMap.end()) {
                auto client_session = it->second;
                if (client_session) {
                    client_session->Send(Protocol::PKT_GATEWAY_CLIENT_MOVE_RES, client_res);
                }
            }
        }
    }
}

// ==========================================
// [추가] GameServer -> Gateway -> Client 전투(피격) 브로드캐스트 릴레이
// ==========================================
void Handle_GameGatewayAttackRes(std::shared_ptr<GameConnection>& session, char* payload, uint16_t size) {
    Protocol::GameGatewayAttackRes s2s_res;
    if (s2s_res.ParseFromArray(payload, size)) {

        // 1. 클라이언트가 읽을 수 있는 AttackRes 패킷으로 변환 (포장지 교체)
        Protocol::AttackRes client_res;
        client_res.set_attacker_uid(s2s_res.attacker_uid());
        client_res.set_target_account_id(s2s_res.target_account_id());
        client_res.set_damage(s2s_res.damage());
        client_res.set_target_remain_hp(s2s_res.target_remain_hp());

        // 2. 이펙트/데미지를 봐야 하는 주변 유저(AOI)들에게 각각 전송
        std::lock_guard<std::mutex> lock(g_clientMutex);
        for (int i = 0; i < s2s_res.target_account_ids_size(); ++i) {
            const std::string& target_id = s2s_res.target_account_ids(i);

            // 현재 게이트웨이 방명록(세션 맵)에 접속해 있는 유저라면 패킷 쏘기!
            auto it = g_clientMap.find(target_id);
            if (it != g_clientMap.end() && it->second) {
                // PKT_GATEWAY_CLIENT_ATTACK_RES (27번) 으로 클라이언트에게 전송
                it->second->Send(Protocol::PKT_GATEWAY_CLIENT_ATTACK_RES, client_res);
            }
        }
    }
}