#include "ClientHandlers.h"
#include "..\\GatewayServer\\GatewayServer.h"
#include "..\\Common\\Protocol\\protocol.pb.h"
#include "..\\GatewayServer\\Session\\ClientSession.h"
#include "..\\GatewayServer\\Network\\GameConnection.h"
#include "..\\Common\\Utils\\Logger.h"

#include <iostream>
#include <mutex>

// ==========================================
//   세션 토큰 검증 + 암호화 활성화
//
// 토큰 검증 성공 후 session->EnableEncryption()을 호출하여
// 이후 모든 패킷을 AES-128-CBC로 암호화합니다.
//
// GatewayConnectRes는 핸드셰이크 응답이므로 평문으로 전송됩니다.
// 클라이언트도 이 응답을 수신한 뒤 동일한 패스프레이즈로 암호화를 활성화합니다.
// ==========================================
void Handle_GatewayConnectReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::GatewayConnectReq req;

    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: GatewayConnectReq (payloadSize=" << payloadSize << ")");
        if (session->OnParseViolation()) {
            session->OnDisconnected();
        }
        return;
    }
    session->OnParseSuccess();

    auto& ctx = GatewayContext::Get();

    // 토큰 검증
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
        UTILITY::LockGuard lock(ctx.clientMutex);
        ctx.clientMap[req.account_id()] = session;
    }

    // 핸드셰이크 응답은 평문으로 전송 (암호화 활성화 전)
    Protocol::GatewayConnectRes res;
    res.set_success(true);
    session->Send(Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES, res);

    //   핸드셰이크 완료 후 암호화 활성화
    // 이 시점 이후 Send/ReadPayload에서 AES-128-CBC 암호화가 적용됩니다.
    // 클라이언트도 GatewayConnectRes 수신 후 동일한 패스프레이즈로 활성화합니다.
    session->EnableEncryption();

    LOG_INFO("Gateway", "유저(" << req.account_id() << ") 토큰 검증 성공, 암호화 활성화, 인게임 입장 승인 완료!");
}

void Handle_ChatReq(std::shared_ptr<ClientSession>& session, char* payload, uint16_t payloadSize) {
    Protocol::ChatReq req;

    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("Gateway", "ParseFromArray 실패: ChatReq (payloadSize=" << payloadSize << ")");
        if (session->OnParseViolation()) {
            session->OnDisconnected();
        }
        return;
    }
    session->OnParseSuccess();

    auto& ctx = GatewayContext::Get();

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
        if (session->OnParseViolation()) {
            session->OnDisconnected();
        }
        return;
    }
    session->OnParseSuccess();

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
        if (session->OnParseViolation()) {
            session->OnDisconnected();
        }
        return;
    }
    session->OnParseSuccess();

    auto& ctx = GatewayContext::Get();
    if (ctx.gameConnection) {
        Protocol::GatewayGameAttackReq s2s_req;
        s2s_req.set_account_id(session->GetAccountId());
        ctx.gameConnection->Send(Protocol::PKT_GATEWAY_GAME_ATTACK_REQ, s2s_req);
    }
}
