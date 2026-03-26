#include "WorldHandlers.h"
#include <iostream>
#include <string>

#include "..\Common\Protocol\protocol.pb.h"
#include "..\Common\Utils\TokenUtils.h"
#include "..\Common\Utils\Logger.h"
#include "../WorldServer.h"

#include "../../Common/ConfigManager.h"

// ==========================================
// 예측 가능한 토큰 → 암호학적 랜덤 토큰 수정
//
// 기존: "WORLD_1_TOKEN_myid" → 예측 가능, 보안 무의미
// 수정: TokenUtils::GenerateSessionToken() → 64자 랜덤 hex
// ==========================================
void Handle_WorldLoginSelectReq(std::shared_ptr<ServerSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::LoginWorldSelectReq req;
    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("WorldServer", "ParseFromArray 실패: LoginWorldSelectReq (payloadSize=" << payloadSize << ")");
        return;
    }

    LOG_INFO("WorldServer", "LoginServer로부터 유저(" << req.account_id() << ")의 월드 " << req.world_id() << "번 입장 요청 수신.");

    Protocol::WorldLoginSelectRes res;
    res.set_account_id(req.account_id());
    res.set_success(true);
    res.set_gateway_ip("127.0.0.1");
    res.set_gateway_port(ConfigManager::GetInstance().GetGatewayServerPort());

    // 보안 토큰 생성 (32바이트 = 64자 hex)
    std::string new_token = TokenUtils::GenerateSessionToken(32);
    res.set_session_token(new_token);

    session->Send(Protocol::PKT_WORLD_LOGIN_SELECT_RES, res);
    LOG_INFO("WorldServer", "유저(" << req.account_id() << ") 보안 토큰 발급 완료.");
}
