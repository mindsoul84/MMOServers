#include "WorldHandlers.h"
#include <iostream>
#include <string>

#include "..\Common\Protocol\protocol.pb.h"
#include "..\Common\Utils\TokenUtils.h"
#include "..\Common\Utils\Logger.h"
#include "../WorldServer.h"

#include "../../Common/ConfigManager.h"

// ==========================================
// 토큰 발급 시 저장소 보관 및 S2S 통지
//
// 1. WorldServer의 TokenStore에 토큰을 저장합니다.
// 2. 연결된 모든 GameServer에 TokenNotify 패킷을 전송합니다.
// 3. GameServer는 이를 받아 GatewayServer로 중계합니다.
// 4. GatewayServer는 클라이언트 접속 시 토큰을 검증합니다.
// ==========================================
void Handle_WorldLoginSelectReq(std::shared_ptr<ServerSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::LoginWorldSelectReq req;
    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("WorldServer", "ParseFromArray 실패: LoginWorldSelectReq (payloadSize=" << payloadSize << ")");
        return;
    }

    LOG_INFO("WorldServer", "LoginServer로부터 유저(" << req.account_id() << ")의 월드 " << req.world_id() << "번 입장 요청 수신.");

    // 보안 토큰 생성 (32바이트 = 64자 hex)
    std::string new_token = TokenUtils::GenerateSessionToken(32);

    // 토큰 저장소에 보관
    g_tokenStore.StoreToken(req.account_id(), new_token);
    int64_t expire_ms = g_tokenStore.GetExpireTime(req.account_id());

    // 연결된 GameServer들에게 토큰 통지 (S2S 릴레이의 첫 단계)
    // GameServer는 이를 받아 GatewayServer에 전달합니다.
    {
        Protocol::TokenNotify token_notify;
        token_notify.set_account_id(req.account_id());
        token_notify.set_session_token(new_token);
        token_notify.set_expire_time_ms(expire_ms);

        UTILITY::LockGuard lock(g_serverSessionMutex);
        for (auto& s : g_serverSessions) {
            if (s) {
                s->Send(Protocol::PKT_WORLD_GAME_TOKEN_NOTIFY, token_notify);
            }
        }
    }
    LOG_INFO("WorldServer", "유저(" << req.account_id() << ") 토큰 발급 및 GameServer 통지 완료.");

    // LoginServer에 응답
    Protocol::WorldLoginSelectRes res;
    res.set_account_id(req.account_id());
    res.set_success(true);
    res.set_gateway_ip("127.0.0.1");
    res.set_gateway_port(ConfigManager::GetInstance().GetGatewayServerPort());
    res.set_session_token(new_token);

    session->Send(Protocol::PKT_WORLD_LOGIN_SELECT_RES, res);
}
