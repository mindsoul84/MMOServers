#include <iostream>
#include <string>
#include <boost/asio.hpp>
#include <windows.h>
#include <thread>
#include <chrono>
#include <vector>
#include <random>
#include <conio.h> // _kbhit(), _getch() 사용을 위해 추가

#include "protocol.pb.h"
#include "Utils/StringUtils.h"
#include "Network/PacketUtils.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

using boost::asio::ip::tcp;

// =======================================================
// [1] 설정 로드 함수
// =======================================================
bool LoadConfigMode() {
    try {
        boost::property_tree::ptree pt;
#ifdef _DEBUG
        boost::property_tree::read_json("../Common/config.json", pt);
#else
        boost::property_tree::read_json("config.json", pt);
#endif        
        return pt.get<bool>("db_conn", false);
    }
    catch (const std::exception& e) {
        std::cerr << "[Warning] config.json 읽기 실패 (기본값 false 적용): " << e.what() << "\n";
        return false;
    }
}

// =======================================================
// [2] 로그인 처리 함수 (아이디/비번 입력 및 LoginServer 연결)
// =======================================================
bool ProcessLogin(tcp::socket& socket, tcp::resolver& resolver, bool use_db, std::string& out_id) {
    std::string my_pw = "";

    while (true) {
        if (use_db) {
            std::cout << "\n========== [ 로그인 ] ==========\n▶ 아이디 입력: ";
            std::cin >> out_id;
            std::cin.ignore(10000, '\n');

            std::cout << "▶ 비밀번호 입력: ";
            my_pw = "";
            while (true) {
                char ch = _getch();
                if (ch == '\r') { std::cout << '\n'; break; }
                else if (ch == '\b') {
                    if (!my_pw.empty()) { std::cout << "\b \b"; my_pw.pop_back(); }
                }
                else { my_pw += ch; std::cout << '*'; }
            }
            std::cout << "==============================\n\n";
        }
        else {
            out_id = GenerateRandomID(6);
            my_pw = GenerateRandomPW(16);
            std::cout << "[System] DB 미사용 모드 - 랜덤 접속 ID 발급: " << out_id << "\n";
        }

        if (socket.is_open()) { boost::system::error_code ec; socket.close(ec); }

        std::cout << "[DummyClient] LoginServer(7777) 연결 중...\n";
        boost::asio::connect(socket, resolver.resolve("127.0.0.1", "7777"));

        Protocol::LoginReq login_req;
        login_req.set_id(out_id);
        login_req.set_password(my_pw);
        login_req.set_input_type(use_db ? 1 : 0);
        SendPacket(socket, Protocol::PKT_CLIENT_LOGIN_LOGIN_REQ, login_req);

        PacketHeader res_header;
        boost::asio::read(socket, boost::asio::buffer(&res_header, sizeof(PacketHeader)));
        std::vector<char> res_payload(res_header.size - sizeof(PacketHeader));
        if (!res_payload.empty()) boost::asio::read(socket, boost::asio::buffer(res_payload.data(), res_payload.size()));

        if (res_header.id == Protocol::PKT_LOGIN_CLIENT_LOGIN_RES) {
            Protocol::LoginRes l_res;
            if (l_res.ParseFromArray(res_payload.data(), res_payload.size()) && l_res.success()) {
                std::cout << "[DummyClient] 계정 로그인 성공! 월드(1) 선택 요청 중...\n";
                return true;
            }
            std::cout << "\n❌ [로그인 실패] 비밀번호가 틀렸거나 이미 접속 중인 계정입니다. 다시 시도해 주세요.\n";
        }
    }
    return false;
}

// =======================================================
// [3] 월드 선택 처리 함수
// =======================================================
bool ProcessWorldSelect(tcp::socket& socket, std::string& out_token, std::string& out_ip, int& out_port) {
    Protocol::WorldSelectReq ws_req;
    ws_req.set_world_id(1);
    SendPacket(socket, Protocol::PKT_CLIENT_LOGIN_WORLD_SELECT_REQ, ws_req);

    PacketHeader res_header;
    boost::asio::read(socket, boost::asio::buffer(&res_header, sizeof(PacketHeader)));
    std::vector<char> res_payload(res_header.size - sizeof(PacketHeader));
    if (!res_payload.empty()) boost::asio::read(socket, boost::asio::buffer(res_payload.data(), res_payload.size()));

    Protocol::WorldSelectRes w_res;
    if (w_res.ParseFromArray(res_payload.data(), res_payload.size()) && w_res.success()) {
        out_token = w_res.session_token();
        out_ip = w_res.gateway_ip();
        out_port = w_res.gateway_port();
        std::cout << "[DummyClient] 🎉 월드 입장 승인 완료! 토큰 발급됨.\n";
        return true;
    }
    return false;
}

// =======================================================
// [4] 백그라운드 수신 스레드 가동 함수
// =======================================================
void StartReceiveThread(tcp::socket& socket, const std::string& my_id, float& my_x, float& my_y, int& my_hp) {
    std::thread recv_thread([&socket, my_id, &my_x, &my_y, &my_hp]() {
        try {
            while (true) {
                PacketHeader h;
                boost::asio::read(socket, boost::asio::buffer(&h, sizeof(PacketHeader)));
                std::vector<char> p(h.size - sizeof(PacketHeader));
                if (!p.empty()) boost::asio::read(socket, boost::asio::buffer(p.data(), p.size()));

                if (h.id == Protocol::PKT_GATEWAY_CLIENT_CHAT_RES) {
                    Protocol::ChatRes chat_res;
                    if (chat_res.ParseFromArray(p.data(), p.size())) {
                        std::cout << "\n[채팅] " << chat_res.account_id() << " : " << chat_res.msg() << "\n";
                    }
                }
                else if (h.id == Protocol::PKT_GATEWAY_CLIENT_MOVE_RES) {
                    Protocol::MoveRes move_res;
                    if (move_res.ParseFromArray(p.data(), p.size()) && move_res.account_id() == my_id) {
                        float distance = std::sqrt(std::pow(my_x - move_res.x(), 2) + std::pow(my_y - move_res.y(), 2));
                        if (distance > 0.1f) {
                            my_x = move_res.x();
                            my_y = move_res.y();
                            if (my_x == 0.0f && my_y == 0.0f && my_hp <= 0) {
                                my_hp = 100;
                                std::cout << "\n✨ [System] 기절하여 서버에 의해 마을로 강제 이동(부활) 되었습니다!\n";
                            }
                            else {
                                std::cout << "\n🚧 [System] 맵의 경계에 도달하여 위치가 보정되었습니다.\n";
                            }

                            std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
                        }
                    }
                }
                else if (h.id == Protocol::PKT_GATEWAY_CLIENT_ATTACK_RES) {
                    Protocol::AttackRes attack_res;
                    if (attack_res.ParseFromArray(p.data(), p.size())) {
                        if (attack_res.damage() == 0) {
                            std::cout << "\n[System] 범위에 벗어나 공격에 실패했습니다.\n";                            
                        }
                        else if (attack_res.target_account_id() == my_id) {
                            my_hp = attack_res.target_remain_hp();
                            std::cout << "\n🩸 [전투] 몬스터에게 " << attack_res.damage() << " 데미지를 입었습니다!\n";
                            if (my_hp <= 0) std::cout << "💀 체력이 0이 되어 기절했습니다...\n";                            
                        }
                        else {
                            std::cout << "\n[Combat] ⚔️ 몬스터(" << attack_res.target_account_id() << ") 타격 성공! 데미지: " << attack_res.damage() << " (남은 체력: " << attack_res.target_remain_hp() << ")\n";

                            if (attack_res.target_remain_hp() <= 0) {
                                std::cout << "🎉 [System] 💀 몬스터(" << attack_res.target_account_id() << ")가 쓰러졌습니다!\n";
                            }
                        }
                        std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
                    }
                }
            }
        }
        catch (...) { std::cout << "\n[서버 연결 종료]\n"; }
        });
    recv_thread.detach();
}

// =======================================================
// [5] 액션 모드 (메인 루프) 함수
// =======================================================
void RunActionLoop(tcp::socket& socket, float& my_x, float& my_y, int& my_hp) {
    std::cout << "\n [액션 모드] 방향키: 이동 / a키: 공격 / Enter: 채팅 / ESC: 종료\n--------------------------------------\n";

    while (true) {
        if (_kbhit()) {
            int key = _getch();
            if (key == 224) { // 방향키
                key = _getch();
                bool moved = false;
                switch (key) {
                case 72: my_y += 1.0f; moved = true; break;
                case 80: my_y -= 1.0f; moved = true; break;
                case 75: my_x -= 1.0f; moved = true; break;
                case 77: my_x += 1.0f; moved = true; break;
                }

                if (moved) {
                    Protocol::MoveReq move_req;
                    move_req.set_x(my_x); move_req.set_y(my_y);
                    SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_MOVE_REQ, move_req);
                    std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
                }
            }
            else if (key == 13) { // Enter 키 (채팅)
                std::cout << "\n[채팅 모드] 입력> ";
                std::string input;
                std::getline(std::cin, input);
                if (!input.empty()) {
                    Protocol::ChatReq chat_req;
                    chat_req.set_msg(AnsiToUtf8(input));
                    SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_CHAT_REQ, chat_req);
                }
                std::cout << "[액션 모드] 방향키: 이동 / a키: 공격 / Enter: 채팅 / ESC: 종료\n";
            }
            else if (key == 27) { // ESC 키
                std::cout << "\n[DummyClient] 접속을 종료합니다.\n";
                break;
            }
            else if (key == 'a' || key == 'A') { // 공격
                Protocol::AttackReq req;
                req.set_target_uid(0);
                SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_ATTACK_REQ, req);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// =======================================================
// [6] 메인 함수
// =======================================================
int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "[DummyClient] Start.. Created by Jeong Shin Young\n";

    bool use_db = LoadConfigMode();
    std::string my_id, session_token, gateway_ip;
    int gateway_port = 0;

    try {
        boost::asio::io_context io_context;
        tcp::resolver resolver(io_context);
        tcp::socket socket(io_context);

        // 1. 로그인
        if (!ProcessLogin(socket, resolver, use_db, my_id)) return 0;

        // 2. 월드 선택
        if (!ProcessWorldSelect(socket, session_token, gateway_ip, gateway_port)) return 0;

        // 3. 인게임(Gateway) 진입
        socket.close();
        std::cout << "--------------------------------------\n[DummyClient] GatewayServer(" << gateway_port << ") 로 게임 진입을 시도합니다...\n";
        socket.open(tcp::v4());
        boost::asio::connect(socket, resolver.resolve(gateway_ip, std::to_string(gateway_port)));

        Protocol::GatewayConnectReq gw_req;
        gw_req.set_account_id(my_id);
        gw_req.set_session_token(session_token);
        SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_CONNECT_REQ, gw_req);

        PacketHeader res_header;
        boost::asio::read(socket, boost::asio::buffer(&res_header, sizeof(PacketHeader)));
        std::vector<char> res_payload(res_header.size - sizeof(PacketHeader));
        if (!res_payload.empty()) boost::asio::read(socket, boost::asio::buffer(res_payload.data(), res_payload.size()));

        if (res_header.id == Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES) {
            std::cout << "======================================\n       🕹️ 인게임 세계에 진입했습니다!       \n======================================\n";
        }

        // 4. 인게임 로직 실행
        float my_x = 0.0f, my_y = 0.0f;
        int my_hp = 100;

        StartReceiveThread(socket, my_id, my_x, my_y, my_hp);
        RunActionLoop(socket, my_x, my_y, my_hp);

        socket.close();
    }
    catch (std::exception& e) { std::cerr << "[Error] " << e.what() << "\n"; }

    return 0;
}