#include "ClientHandlers.h"
#include "../LoginServer/LoginServer.h"
#include "..\Common\Protocol\protocol.pb.h"
#include "..\Common\ConfigManager.h"
#include "..\Common\DB\DBManager.h"
#include <iostream>

void Handle_LoginReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::LoginReq login_req;
    if (login_req.ParseFromArray(payload, payloadSize)) {
        std::string req_id = login_req.id();
        std::string req_pw = login_req.password();
        int req_input_type = login_req.input_type();
        Protocol::LoginRes login_res;

        bool is_auth_success = false;

        if (ConfigManager::GetInstance().UseDB()) {
            LoginResult result = DBManager::GetInstance().ProcessLogin(req_id, req_pw, req_input_type);
            if (result == LoginResult::SUCCESS) {
                std::cout << "[DB] 계정 인증 성공 (" << req_id << ")\n";
                is_auth_success = true;
            }
            else if (result == LoginResult::NEW_REGISTERED) {
                std::cout << "[DB] 신규 계정 자동 가입 완료 (" << req_id << ")\n";
                is_auth_success = true;
            }
            else if (result == LoginResult::WRONG_PASSWORD) {
                std::cout << "🚨 [DB] 로그인 실패: 비밀번호 불일치 (" << req_id << ")\n";
            }
            else {
                std::cout << "🚨 [DB] 로그인 실패: DB 쿼리 오류\n";
            }
        }
        else {
            is_auth_success = true; // DB 미사용 모드
        }

        if (is_auth_success) {
            std::lock_guard<std::mutex> lock(g_loginMutex);
            if (g_loggedInUsers.find(req_id) != g_loggedInUsers.end()) {
                login_res.set_success(false);
                std::cout << "[로그인 거부] 이미 접속 중인 계정: " << req_id << "\n";
            }
            else {
                g_loggedInUsers.insert(req_id);
                g_sessionMap[req_id] = session;
                session->SetLoggedInId(req_id);
                login_res.set_success(true);
                std::cout << "[로그인 승인] ID: " << req_id << " 인게임 진입 허용.\n";
            }
        }
        else {
            login_res.set_success(false);
        }

        session->Send(Protocol::PKT_LOGIN_CLIENT_LOGIN_RES, login_res);
    }
}

void Handle_Heartbeat(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::Heartbeat hb;
    if (hb.ParseFromArray(payload, payloadSize)) {
        std::cout << "[패킷수신] PKT_HEARTBEAT - 클라이언트(" << session->GetLoggedInId() << ") 생존 확인!\n";
    }
}

void Handle_WorldSelectReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::WorldSelectReq req;
    if (req.ParseFromArray(payload, payloadSize)) {
        std::cout << "[LoginServer] 유저(" << session->GetLoggedInId() << ")가 월드 " << req.world_id() << "번 선택.\n";

        if (g_worldConnection) {
            Protocol::LoginWorldSelectReq s2s_req;
            s2s_req.set_account_id(session->GetLoggedInId());
            s2s_req.set_world_id(req.world_id());
            g_worldConnection->Send(Protocol::PKT_LOGIN_WORLD_SELECT_REQ, s2s_req);
        }
    }
}