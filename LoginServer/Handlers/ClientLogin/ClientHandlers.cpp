#include "ClientHandlers.h"
#include "../../Session/Session.h"
#include "../../Network/WorldConnection.h"
#include "../LoginServer/LoginServer.h"
#include "..\Common\Protocol\protocol.pb.h"
#include "..\Common\ConfigManager.h"
#include "..\Common\DB\DBManager.h"
#include <iostream>
#include <boost/asio/post.hpp> // ★ 추가

void Handle_LoginReq(std::shared_ptr<Session>& session, char* payload, uint16_t payloadSize) {
    auto login_req = std::make_shared<Protocol::LoginReq>();
    if (!login_req->ParseFromArray(payload, payloadSize))
        return;

    // ★ 1. DB 스레드 풀(Task Queue)에 무거운 작업 위임 (네트워크 스레드는 즉시 리턴!)
    boost::asio::post(g_db_io_context, [session, login_req]() {

        // ---------------------------------------------------------
        // [이 구간은 DB 전담 스레드 내부입니다]
        // 여기서 DB가 3초간 멈춰도 다른 유저들의 채팅이나 로그인 시도는 전혀 끊기지 않습니다!
        // ---------------------------------------------------------
        std::string req_id = login_req->id();
        std::string req_pw = login_req->password();
        int req_input_type = login_req->input_type();

        bool is_auth_success = false;

        if (ConfigManager::GetInstance().UseDB()) {
            // =============================================================================================================================
            // ★ [수정] 싱글톤이 아닌, '현재 이 코드를 실행 중인 DB 스레드'의 전용 DBManager 객체(t_dbManager)를 사용하여 쿼리를 날립니다!
            // =============================================================================================================================
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
            }
            else {
                std::cout << "🚨 [DB] DB 연결 객체가 할당되지 않았습니다.\n";
            }
        }
        else {
            is_auth_success = true; // DB 미사용 모드
        }

        // ★ 2. DB 연산이 끝났으므로, 결과를 유저(Session)에게 돌려줍니다.
        // Session->Send() 내부에 비동기 전송 로직이 있으므로 이 안에서 바로 호출해도 안전합니다.
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