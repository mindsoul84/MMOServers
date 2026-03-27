#include "ClientHandlers.h"
#include "../../Session/Session.h"
#include "../../Network/WorldConnection.h"
#include "../LoginServer/LoginServer.h"
#include "..\\Common\\Protocol\\protocol.pb.h"
#include "..\\Common\\ConfigManager.h"
#include "..\\Common\\DB\\DBManager.h"
#include <iostream>
#include <boost/asio/post.hpp>

void Handle_LoginReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    auto login_req = std::make_shared<Protocol::LoginReq>();

    // [수정] ParseFromArray 실패 시 로그 출력 후 반환
    if (!login_req->ParseFromArray(payload, payloadSize)) {
        std::cerr << "[LoginServer] 🚨 ParseFromArray 실패: LoginReq (payloadSize=" << payloadSize << ")\n";
        return;
    }

    boost::asio::post(g_db_io_context, [session, login_req]() {
        std::string req_id = login_req->id();
        std::string req_pw = login_req->password();
        int req_input_type = login_req->input_type();

        bool is_auth_success = false;

        if (ConfigManager::GetInstance().UseDB()) {
            if (t_dbManager) {
                LoginResult result = t_dbManager->ProcessLogin(req_id, req_pw, req_input_type);

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
                    std::cerr << "🚨 [DB] DB 오류로 로그인 처리 실패 (" << req_id << ")\n";
                }
            }
            else {
                std::cerr << "🚨 [DB] DB 연결 객체가 할당되지 않았습니다.\n";
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
        });
}

void Handle_Heartbeat(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::Heartbeat hb;

    // [수정] ParseFromArray 실패 시 로그 출력
    if (!hb.ParseFromArray(payload, payloadSize)) {
        std::cerr << "[LoginServer] 🚨 ParseFromArray 실패: Heartbeat (payloadSize=" << payloadSize << ")\n";
        return;
    }

    // [수정] Heartbeat 수신 시 타임스탬프 갱신 (타임아웃 카운터 리셋)
    session->UpdateHeartbeat();

    std::cout << "[패킷수신] PKT_HEARTBEAT - 클라이언트(" << session->GetLoggedInId() << ") 생존 확인!\n";
}

void Handle_WorldSelectReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    Protocol::WorldSelectReq req;

    // [수정] ParseFromArray 실패 시 로그 출력
    if (!req.ParseFromArray(payload, payloadSize)) {
        std::cerr << "[LoginServer] 🚨 ParseFromArray 실패: WorldSelectReq (payloadSize=" << payloadSize << ")\n";
        return;
    }

    std::cout << "[LoginServer] 유저(" << session->GetLoggedInId() << ")가 월드 " << req.world_id() << "번 선택.\n";

    if (g_worldConnection) {
        Protocol::LoginWorldSelectReq s2s_req;
        s2s_req.set_account_id(session->GetLoggedInId());
        s2s_req.set_world_id(req.world_id());
        g_worldConnection->Send(Protocol::PKT_LOGIN_WORLD_SELECT_REQ, s2s_req);
    }
}
