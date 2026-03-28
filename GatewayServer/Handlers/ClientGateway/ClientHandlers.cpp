#include "ClientHandlers.h"
#include "..\\GatewayServer\\GatewayServer.h"
#include "..\\Common\\Protocol\\protocol.pb.h"
#include "..\\GatewayServer\\Session\\ClientSession.h"
#include "..\\GatewayServer\\Network\\GameConnection.h"
#include "..\\Common\\Utils\\Logger.h"

#include <iostream>
#include <mutex>

// ==========================================
// [수정] 세션 토큰 검증 추가
//
// 변경 전: 토큰을 받기만 하고 아무 확인 없이 success 반환
//   -> 아무 문자열이나 보내도 접속 가능 (보안 결함)
//
// 변경 후: pendingTokens에서 토큰을 검색하여 일치 여부 확인
//   -> 일치하지 않거나 만료된 경우 접속 거부
//   -> 검증 성공 시 일회용으로 토큰 소비 (재사용 방지)
// ==========================================
void Handle_GatewayConnectReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::GatewayConnectReq req;

    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: GatewayConnectReq (payloadSize=" << payloadSize << ")");
        return;
    }

    auto& ctx = GatewayContext::Get();

    // [추가] 토큰 검증
    bool token_valid = ctx.VerifyAndConsumeToken(req.account_id(), req.session_token());
    if (!token_valid) {
        LOG_WARN("Gateway", "토큰 검증 실패! (유저: " << req.account_id() << ") - 접속 거부");

        Protocol::GatewayConnectRes res;
        res.set_success(false);
        res.set_reason("Invalid or expired session token");
        session->Send(Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES, res);
        return;
    }

    session->SetAccountId(req.account_id());

    {
        std::lock_guard<std::mutex> lock(ctx.clientMutex);
        ctx.clientMap[req.account_id()] = session;
    }

    Protocol::GatewayConnectRes res;
    res.set_success(true);
    session->Send(Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES, res);
    LOG_INFO("Gateway", "유저(" << req.account_id() << ") 토큰 검증 성공, 인게임 입장 승인 완료!");
}

// ==========================================
// [수정] 채팅을 GameServer로 라우팅 (AOI 기반)
//
// 변경 전: clientMap 전체를 순회하며 모든 유저에게 브로드캐스트
//   -> 동접 수천~수만 명일 때 O(N) 비용으로 즉시 병목
//
// 변경 후: GameServer의 Zone/AOI 시스템을 활용
//   -> S2S로 채팅을 GameServer에 전송
//   -> GameServer가 발신자 위치 기반 AOI 내 유저 목록 계산
//   -> AOI 대상자 목록과 함께 응답
//   -> 해당 유저들에게만 채팅 전달 (O(K), K = AOI 내 유저)
// ==========================================
void Handle_ChatReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::ChatReq req;

    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: ChatReq (payloadSize=" << payloadSize << ")");
        return;
    }

    auto& ctx = GatewayContext::Get();

    // GameServer로 S2S 채팅 요청 전송
    if (ctx.gameConnection) {
        Protocol::GatewayGameChatReq s2s_req;
        s2s_req.set_account_id(session->GetAccountId());
        s2s_req.set_msg(req.msg());
        ctx.gameConnection->Send(Protocol::PKT_GATEWAY_GAME_CHAT_REQ, s2s_req);
    }
}

void Handle_MoveReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::MoveReq req;

    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: MoveReq (payloadSize=" << payloadSize << ")");
        return;
    }

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

void Handle_AttackReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::AttackReq req;

    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: AttackReq (payloadSize=" << payloadSize << ")");
        return;
    }

    auto& ctx = GatewayContext::Get();
    if (ctx.gameConnection) {
        Protocol::GatewayGameAttackReq s2s_req;
        s2s_req.set_account_id(session->GetAccountId());
        ctx.gameConnection->Send(Protocol::PKT_GATEWAY_GAME_ATTACK_REQ, s2s_req);
    }
}
