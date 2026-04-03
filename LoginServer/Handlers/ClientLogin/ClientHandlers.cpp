#include "ClientHandlers.h"
#include "../../Session/Session.h"
#include "../../Network/WorldConnection.h"
#include "../LoginServer/LoginServer.h"
#include "..\\Common\\Protocol\\protocol.pb.h"
#include "..\\Common\\ConfigManager.h"
#include "..\\Common\\Utils\\Logger.h"
#include <iostream>
#include <boost/asio/post.hpp>

// ==========================================
//   Handle_LoginReq — DBConnectionPool 기반으로 전환
//
// 변경 전: thread_local t_dbManager->ProcessLogin(...) 직접 호출
//   -> DB 스레드와 연결이 1:1로 묶여 있어 확장성 제한
//
// 변경 후: ctx.db_pool_.Acquire()로 연결 획득, ScopedConnection RAII 반납
//   -> 풀에서 사용 가능한 연결을 동적으로 할당
//   -> 풀 고갈 시 타임아웃(5초) 후 로그인 실패 응답
//   -> ScopedConnection이 스코프 종료 시 자동 반납하여 누수 방지
// ==========================================
void Handle_LoginReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    auto login_req = std::make_shared<Protocol::LoginReq>();

    if (!login_req->ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("LoginServer", "ParseFromArray 실패: LoginReq (payloadSize=" << payloadSize << ")");
        return;
    }

    boost::asio::post(g_db_io_context, [session, login_req]() {
        std::string req_id = login_req->id();
        std::string req_pw = login_req->password();
        int req_input_type = login_req->input_type();

        bool is_auth_success = false;
        int64_t account_uid = 0;

        if (ConfigManager::GetInstance().UseDB()) {
            auto& ctx = LoginContext::Get();

            //   연결 풀에서 ODBC 연결 획득 (최대 5초 대기)
            auto conn = ctx.db_pool_.Acquire(5000);
            if (!conn) {
                LOG_ERROR("DB", "DB 연결 풀 고갈 - 로그인 처리 불가 (유저: " << req_id << ")");
                Protocol::LoginRes login_res;
                login_res.set_success(false);
                session->Send(Protocol::PKT_LOGIN_CLIENT_LOGIN_RES, login_res);
                return;
            }

            //   conn-> 으로 ScopedConnection을 통해 DBManager 메서드 호출
            LoginResult result = conn->ProcessLogin(req_id, req_pw, req_input_type, &account_uid);

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
            // conn은 여기서 스코프를 벗어나며 자동으로 풀에 반납됨
        }
        else {
            is_auth_success = true;
        }

        Protocol::LoginRes login_res;

        if (is_auth_success) {
            UTILITY::LockGuard lock(g_loginMutex);
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

    session->UpdateHeartbeat();

    LOG_TRACE("LoginServer", "Heartbeat 수신: " << session->GetLoggedInId());
}

void Handle_WorldSelectReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::WorldSelectReq req;

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
