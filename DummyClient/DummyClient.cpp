#include <iostream>
#include <string>
#include <boost/asio.hpp>
#include <windows.h>
#include <thread>
#include <chrono>
#include <vector>
#include <random>
#include <conio.h>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "Utils/StringUtils.h"
#include "Network/PacketUtils.h"
#include "Handlers/GatewayHandlers.h"

#include "..\Common\ConfigManager.h"
#include "..\Common\Network\PacketCrypto.h"
#include "..\Common\Define\SecurityConstants.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

using boost::asio::ip::tcp;

// ==========================================
//   클라이언트 측 패킷 암호화 상태
//
// GatewayConnectRes 성공 수신 후 사전 공유 패스프레이즈로 초기화됩니다.
// 이후 모든 Gateway 통신에서 SendPacket의 crypto 파라미터로 전달되고,
// 수신 스레드에서 복호화에 사용됩니다.
//
// LoginServer 통신에는 적용되지 않습니다 (crypto 파라미터 미전달).
// ==========================================
static PacketCrypto g_crypto;
static bool g_crypto_enabled = false;

// =======================================================
// [1] 로그인 처리 함수 (LoginServer 통신 — 평문)
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

        short login_port = ConfigManager::GetInstance().GetDummyClientLoginPort();
        std::cout << "[DummyClient] LoginServer(" << login_port << ") 연결 중...\n";
        boost::asio::connect(socket, resolver.resolve("127.0.0.1", std::to_string(login_port)));

        Protocol::LoginReq login_req;
        login_req.set_id(out_id);
        login_req.set_password(my_pw);
        login_req.set_input_type(use_db ? 1 : 0);
        SendPacket(socket, Protocol::PKT_CLIENT_LOGIN_LOGIN_REQ, login_req);  // 평문 (crypto 미전달)

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
            std::cout << "\n[로그인 실패] 비밀번호가 틀렸거나 이미 접속 중인 계정입니다. 다시 시도해 주세요.\n";
        }
    }
    return false;
}

// =======================================================
// [2] 월드 선택 처리 함수 (LoginServer 통신 — 평문)
// =======================================================
bool ProcessWorldSelect(tcp::socket& socket, std::string& out_token, std::string& out_ip, int& out_port) {
    Protocol::WorldSelectReq ws_req;
    ws_req.set_world_id(1);
    SendPacket(socket, Protocol::PKT_CLIENT_LOGIN_WORLD_SELECT_REQ, ws_req);  // 평문

    PacketHeader res_header;
    boost::asio::read(socket, boost::asio::buffer(&res_header, sizeof(PacketHeader)));
    std::vector<char> res_payload(res_header.size - sizeof(PacketHeader));
    if (!res_payload.empty()) boost::asio::read(socket, boost::asio::buffer(res_payload.data(), res_payload.size()));

    Protocol::WorldSelectRes w_res;
    if (w_res.ParseFromArray(res_payload.data(), res_payload.size()) && w_res.success()) {
        out_token = w_res.session_token();
        out_ip = w_res.gateway_ip();
        out_port = w_res.gateway_port();
        std::cout << "[DummyClient] 월드 입장 승인 완료! 토큰 발급됨.\n";
        return true;
    }
    return false;
}

// =======================================================
// [3] 백그라운드 수신 스레드 — 암호화 복호화 통합
// =======================================================
std::thread StartReceiveThread(tcp::socket& socket, const std::string& my_id, float& my_x, float& my_y, int& my_hp, std::unordered_map<std::string, std::pair<float, float>>& monster_pos_map) {
    std::thread recv_thread([&socket, my_id, &my_x, &my_y, &my_hp, &monster_pos_map]() {
        try {
            while (true) {
                PacketHeader h;
                boost::asio::read(socket, boost::asio::buffer(&h, sizeof(PacketHeader)));
                std::vector<char> p(h.size - sizeof(PacketHeader));
                if (!p.empty()) boost::asio::read(socket, boost::asio::buffer(p.data(), p.size()));

                //   암호화 활성 시 수신 페이로드 복호화
                if (g_crypto_enabled && g_crypto.IsInitialized() && !p.empty()) {
                    auto result = g_crypto.Decrypt(p.data(), static_cast<uint16_t>(p.size()));
                    if (result.success) {
                        p = std::move(result.data);
                    }
                    else {
                        std::cerr << "[DummyClient] 패킷 복호화 실패: " << result.error_message << "\n";
                        continue;
                    }
                }

                if (h.id == Protocol::PKT_GATEWAY_CLIENT_CHAT_RES) {
                    Protocol::ChatRes chat_res;
                    if (chat_res.ParseFromArray(p.data(), p.size())) {
                        std::cout << "\n[채팅] " << chat_res.account_id() << " : " << chat_res.msg() << "\n";
                    }
                }
                else if (h.id == Protocol::PKT_GATEWAY_CLIENT_MOVE_RES)
                {
                    HandleMoveRes(p, my_id, my_x, my_y, my_hp, monster_pos_map);
                }
                else if (h.id == Protocol::PKT_GATEWAY_CLIENT_ATTACK_RES)
                {
                    HandleAttackRes(p, my_id, my_x, my_y, my_hp, monster_pos_map);
                }
            }
        }
        catch (...) { std::cout << "\n[서버 연결 종료]\n"; }
        });
    return recv_thread;
}

// =======================================================
// [4] 액션 모드 — 암호화된 패킷 전송
// =======================================================
void RunActionLoop(tcp::socket& socket, float& my_x, float& my_y, int& my_hp) {
    std::cout << "\n [액션 모드] 방향키: 이동 / a키: 공격 / Enter: 채팅 / ESC: 종료\n--------------------------------------\n";

    const int KEY_SPECIAL = 224;
    const int KEY_UP      = 72;
    const int KEY_DOWN    = 80;
    const int KEY_LEFT    = 75;
    const int KEY_RIGHT   = 77;
    const int KEY_ENTER   = 13;
    const int KEY_ESC     = 27;

    //   암호화 활성 시 g_crypto 포인터를 SendPacket에 전달
    PacketCrypto* crypto_ptr = g_crypto_enabled ? &g_crypto : nullptr;

    while (true) {
        if (_kbhit()) {
            int key = _getch();
            if (key == KEY_SPECIAL) {
                key = _getch();
                bool moved = false;
                switch (key)
                {
                    case KEY_UP:    my_y += 1.0f; moved = true; break;
                    case KEY_DOWN:  my_y -= 1.0f; moved = true; break;
                    case KEY_LEFT:  my_x -= 1.0f; moved = true; break;
                    case KEY_RIGHT: my_x += 1.0f; moved = true; break;
                }

                if (moved) {
                    Protocol::MoveReq move_req;
                    move_req.set_x(my_x); move_req.set_y(my_y);
                    SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_MOVE_REQ, move_req, crypto_ptr);
                    std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
                }
            }
            else if (key == KEY_ENTER) {
                std::cout << "\n[채팅 모드] 입력> ";
                std::string input;
                std::getline(std::cin, input);
                if (!input.empty()) {
                    Protocol::ChatReq chat_req;
                    chat_req.set_msg(AnsiToUtf8(input));
                    SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_CHAT_REQ, chat_req, crypto_ptr);
                }
                std::cout << "[액션 모드] 방향키: 이동 / a키: 공격 / Enter: 채팅 / ESC: 종료\n";
            }
            else if (key == KEY_ESC) {
                std::cout << "\n[DummyClient] 접속을 종료합니다.\n";
                break;
            }
            else if (key == 'a' || key == 'A') {
                Protocol::AttackReq req;
                req.set_target_uid(0);
                SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_ATTACK_REQ, req, crypto_ptr);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// =======================================================
// [5] 메인 함수
// =======================================================
int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "[DummyClient] Start.. Created by Jeong Shin Young\n";

    if (!ConfigManager::GetInstance().LoadConfig("config.json"))
    {
        std::cerr << "config 설정 파일 오류로 인해 DummyClient 종료합니다.\n";
        system("pause");
        return -1;
    }

    bool use_db = ConfigManager::GetInstance().UseDB();
    std::string my_id, session_token, gateway_ip;
    int gateway_port = 0;

    try {
        boost::asio::io_context io_context;
        tcp::resolver resolver(io_context);
        tcp::socket socket(io_context);

        // 1. 로그인 (LoginServer — 평문 통신)
        if (!ProcessLogin(socket, resolver, use_db, my_id)) return 0;

        // 2. 월드 선택 (LoginServer — 평문 통신)
        if (!ProcessWorldSelect(socket, session_token, gateway_ip, gateway_port)) return 0;

        // 3. 인게임(Gateway) 진입
        socket.close();
        std::cout << "--------------------------------------\n[DummyClient] GatewayServer(" << gateway_port << ") 로 게임 진입을 시도합니다...\n";
        socket.open(tcp::v4());
        boost::asio::connect(socket, resolver.resolve(gateway_ip, std::to_string(gateway_port)));

        // 핸드셰이크 요청 (평문 — 암호화 활성화 전)
        Protocol::GatewayConnectReq gw_req;
        gw_req.set_account_id(my_id);
        gw_req.set_session_token(session_token);
        SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_CONNECT_REQ, gw_req);  // crypto 미전달 = 평문

        PacketHeader res_header;
        boost::asio::read(socket, boost::asio::buffer(&res_header, sizeof(PacketHeader)));
        std::vector<char> res_payload(res_header.size - sizeof(PacketHeader));
        if (!res_payload.empty()) boost::asio::read(socket, boost::asio::buffer(res_payload.data(), res_payload.size()));

        // 핸드셰이크 응답도 평문 (서버가 응답 전송 후 암호화를 활성화하므로)
        if (res_header.id == Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES) {
            Protocol::GatewayConnectRes gw_res;
            if (gw_res.ParseFromArray(res_payload.data(), res_payload.size()) && gw_res.success()) {
                std::cout << "======================================\n       [인게임 세계에 진입했습니다!]       \n======================================\n";

                //   핸드셰이크 성공 후 암호화 활성화
                // 서버(ClientSession)도 이 시점에 동일한 패스프레이즈로 활성화했으므로
                // 이후 모든 패킷은 양쪽에서 암호화/복호화됩니다.
                if (g_crypto.InitializeWithPassphrase(SecurityConstants::Crypto::SHARED_PASSPHRASE)) {
                    g_crypto_enabled = true;
                    std::cout << "[DummyClient] 패킷 암호화 활성화 (AES-128-CBC)\n";
                }
                else {
                    std::cerr << "[DummyClient] 패킷 암호화 초기화 실패 - 평문 통신으로 계속\n";
                }
            }
            else {
                std::string reason = gw_res.reason().empty() ? "알 수 없는 오류" : gw_res.reason();
                std::cerr << "\n[접속 실패] 게이트웨이 인증 거부: " << reason << "\n";
                std::cerr << "서버를 재시작하거나 잠시 후 다시 시도해 주세요.\n";
                system("pause");
                return 0;
            }
        }

        // 4. 인게임 로직 실행
        float my_x = 0.0f, my_y = 0.0f;
        int my_hp = 100;
        
        std::unordered_map<std::string, std::pair<float, float>> monster_pos_map;

        auto recv_thread = StartReceiveThread(socket, my_id, my_x, my_y, my_hp, monster_pos_map);
        RunActionLoop(socket, my_x, my_y, my_hp);

        socket.close();
        if (recv_thread.joinable()) recv_thread.join();
    }
    catch (std::exception& e) { std::cerr << "[Error] " << e.what() << "\n"; }

    return 0;
}
