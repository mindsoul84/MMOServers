#include "ClientHandlers.h"
#include "../../Session/Session.h"
#include "../../Network/WorldConnection.h"
#include "../LoginServer/LoginServer.h"
#include "..\\Common\\Protocol\\protocol.pb.h"
#include "..\\Common\\ConfigManager.h"
#include "..\\Common\\DB\\DBManager.h"
#include "..\\Common\\Utils\\Logger.h"
#include <iostream>
#include <boost/asio/post.hpp>

void Handle_LoginReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    auto login_req = std::make_shared<Protocol::LoginReq>();

    // [수정] ParseFromArray 실패 시 로그 출력 후 반환
    if (!login_req->ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("LoginServer", "ParseFromArray 실패: LoginReq (payloadSize=" << payloadSize << ")");
        return;
    }

    boost::asio::post(g_db_io_context, [session, login_req]() {
        std::string req_id = login_req->id();
        std::string req_pw = login_req->password();
        int req_input_type = login_req->input_type();

        bool is_auth_success = false;

        // [수정] DB 스키마 변경에 따라 account_uid를 함께 수신
        int64_t account_uid = 0;

        if (ConfigManager::GetInstance().UseDB()) {
            if (t_dbManager) {
                LoginResult result = t_dbManager->ProcessLogin(req_id, req_pw, req_input_type, &account_uid);

                if (result == LoginResult::SUCCESS) {
                    LOG_INFO("DB", "계정 인증 성공 (" << req_id << ", UID:" << account_uid << ")");
                    is_auth_success = true;
                }
                else if (result == LoginResult::NEW_REGISTERED) {
                    LOG_INFO("DB", "신규 계정 자동 가입 완료 (" << req_id << ", UID:" << account_uid << ")");
                    is_auth_success = true;
                }
                else if (result == LoginResult::WRONG_PASSWORD) {
                    LOG_WARN("DB", "로그인 실패: 비밀번호 불일치 (" << req_id << ")");
                }
                else {
                    LOG_ERROR("DB", "DB 오류로 로그인 처리 실패 (" << req_id << ")");
                }
            }
            else {
                LOG_ERROR("DB", "DB 연결 객체가 할당되지 않았습니다.");
            }
        }
        else {
            is_auth_success = true;
        }

        Protocol::LoginRes login_res;

        if (is_auth_success) {
            std::lock_guard<std::mutex> lock(g_loginMutex);
            if (g_loggedInUsers.find(req_id) != g_loggedInUsers.end()) {
                login_res.set_success(false);
                LOG_WARN("LoginServer", "이미 접속 중인 계정: " << req_id);
            }
            else {
                g_loggedInUsers.insert(req_id);
                g_sessionMap[req_id] = session;
                session->SetLoggedInId(req_id);
                login_res.set_success(true);
                LOG_INFO("LoginServer", "로그인 승인 (ID: " << req_id << ", account_uid: " << account_uid << ")");
            }
        }
        else {
            login_res.set_success(false);
        }

        session->Send(Protocol::PKT_LOGIN_CLIENT_LOGIN_RES, login_res);
        });
}

void Handle_Heartbeat(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::Heartbeat hb;

    if (!hb.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("LoginServer", "ParseFromArray 실패: Heartbeat (payloadSize=" << payloadSize << ")");
        return;
    }

    // [수정] Heartbeat 수신 시 타임스탬프 갱신 (타임아웃 카운터 리셋)
    session->UpdateHeartbeat();

    LOG_TRACE("LoginServer", "Heartbeat 수신: " << session->GetLoggedInId());
}

void Handle_WorldSelectReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::WorldSelectReq req;

    // [수정] ParseFromArray 실패 시 로그 출력
    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("LoginServer", "ParseFromArray 실패: WorldSelectReq (payloadSize=" << payloadSize << ")");
        return;
    }

    LOG_INFO("LoginServer", "유저(" << session->GetLoggedInId() << ")가 월드 " << req.world_id() << "번 선택.");

    if (g_worldConnection) {
        Protocol::LoginWorldSelectReq s2s_req;
        s2s_req.set_account_id(session->GetLoggedInId());
        s2s_req.set_world_id(req.world_id());
        g_worldConnection->Send(Protocol::PKT_LOGIN_WORLD_SELECT_REQ, s2s_req);
    }
}
