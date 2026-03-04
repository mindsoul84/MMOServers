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

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

using boost::asio::ip::tcp;

// =================================================
// 윈도우 콘솔 문자열(CP949)을 UTF-8로 변환하는 함수
// =================================================
std::string AnsiToUtf8(const std::string& ansiStr) {
    if (ansiStr.empty()) return "";
    int wLen = MultiByteToWideChar(CP_ACP, 0, ansiStr.c_str(), -1, NULL, 0);
    std::wstring wStr(wLen, 0);
    MultiByteToWideChar(CP_ACP, 0, ansiStr.c_str(), -1, &wStr[0], wLen);
    int uLen = WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string utf8Str(uLen - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), -1, &utf8Str[0], uLen, NULL, NULL);
    return utf8Str;
}

std::string GenerateRandomID(int length) {
    const std::string CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<> dist(0, CHARS.size() - 1);
    std::string random_string;
    for (int i = 0; i < length; ++i) random_string += CHARS[dist(generator)];
    return random_string;
}

// 패킷 전송 헬퍼 함수
void SendPacket(tcp::socket& socket, uint16_t pktId, const google::protobuf::Message& msg) {
    std::string payload;
    msg.SerializeToString(&payload);
    PacketHeader header;
    header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
    header.id = pktId;

    std::vector<char> send_buffer(header.size);
    memcpy(send_buffer.data(), &header, sizeof(PacketHeader));
    memcpy(send_buffer.data() + sizeof(PacketHeader), payload.data(), payload.size());
    boost::asio::write(socket, boost::asio::buffer(send_buffer));
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::string my_id = GenerateRandomID(6);
    std::string session_token = "";
    std::string gateway_ip = "";
    int gateway_port = 0;

    std::cout << "[DummyClient] Start.. Created by Jeong Shin Young\n";

    try {
        boost::asio::io_context io_context;
        tcp::resolver resolver(io_context);
        tcp::socket socket(io_context);

        // ==========================================
        // LoginServer 접속 및 월드 선택 과정
        // ==========================================
        std::cout << "[DummyClient] LoginServer(7777) 연결 중...\n";
        boost::asio::connect(socket, resolver.resolve("127.0.0.1", "7777"));

        Protocol::LoginReq login_req;
        login_req.set_id(my_id);
        login_req.set_password("1234");
        SendPacket(socket, Protocol::PKT_CLIENT_LOGIN_LOGIN_REQ, login_req);

        PacketHeader res_header;
        boost::asio::read(socket, boost::asio::buffer(&res_header, sizeof(PacketHeader)));
        std::vector<char> res_payload(res_header.size - sizeof(PacketHeader));
        if (!res_payload.empty()) boost::asio::read(socket, boost::asio::buffer(res_payload.data(), res_payload.size()));

        if (res_header.id == Protocol::PKT_LOGIN_CLIENT_LOGIN_RES) {
            std::cout << "[DummyClient] 계정 로그인 성공! 월드(1) 선택 요청 중...\n";
            Protocol::WorldSelectReq ws_req;
            ws_req.set_world_id(1);
            SendPacket(socket, Protocol::PKT_CLIENT_LOGIN_WORLD_SELECT_REQ, ws_req);

            boost::asio::read(socket, boost::asio::buffer(&res_header, sizeof(PacketHeader)));
            res_payload.resize(res_header.size - sizeof(PacketHeader));
            if (!res_payload.empty()) boost::asio::read(socket, boost::asio::buffer(res_payload.data(), res_payload.size()));

            Protocol::WorldSelectRes w_res;
            if (w_res.ParseFromArray(res_payload.data(), res_payload.size()) && w_res.success()) {
                session_token = w_res.session_token();
                gateway_ip = w_res.gateway_ip();
                gateway_port = w_res.gateway_port();
                std::cout << "[DummyClient] 🎉 월드 입장 승인 완료! 토큰 발급됨.\n";
            }
            else return 0;
        }

        // ==========================================
        // LoginServer 연결 종료 및 GatewayServer 연결
        // ==========================================
        socket.close();
        std::cout << "--------------------------------------\n";
        std::cout << "[DummyClient] GatewayServer(" << gateway_port << ") 로 게임 진입을 시도합니다...\n";

        socket.open(tcp::v4());
        boost::asio::connect(socket, resolver.resolve(gateway_ip, std::to_string(gateway_port)));

        Protocol::GatewayConnectReq gw_req;
        gw_req.set_account_id(my_id);
        gw_req.set_session_token(session_token);
        SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_CONNECT_REQ, gw_req);

        boost::asio::read(socket, boost::asio::buffer(&res_header, sizeof(PacketHeader)));
        res_payload.resize(res_header.size - sizeof(PacketHeader));
        if (!res_payload.empty()) boost::asio::read(socket, boost::asio::buffer(res_payload.data(), res_payload.size()));

        if (res_header.id == Protocol::PKT_GATEWAY_CLIENT_CONNECT_RES) {
            std::cout << "======================================\n";
            std::cout << "       🕹️ 인게임 세계에 진입했습니다!       \n";
            std::cout << "======================================\n";
        }

        // ================================================
        // 인게임 통신: 수신 스레드와 메인 루프 (모드 전환)
        // ================================================

        // 수신 스레드와 메인 스레드가 함께 공유할 내 상태 변수들
        float my_x = 0.0f;
        float my_y = 0.0f;
        int my_hp = 100;

        // [서버 패킷 수신 전용 백그라운드 스레드]
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
                        if (move_res.ParseFromArray(p.data(), p.size())) {
                            if (move_res.account_id() == my_id) {
                                float dx = my_x - move_res.x();
                                float dy = my_y - move_res.y();
                                float distance = std::sqrt(dx * dx + dy * dy);

                                // 내가 걸어간 거라면 거리가 0입니다. 
                                // 0.1 이상 차이가 나면 서버가 강제로 좌표를 덮어씌운 것(텔레포트)입니다!
                                if (distance > 0.1f) {
                                    my_x = move_res.x();
                                    my_y = move_res.y();

                                    // ★ [추가] 마을(0,0)로 부활한 것이라면 체력도 100으로 가득 채워줍니다.
                                    if (my_x == 0.0f && my_y == 0.0f) {
                                        my_hp = 100;
                                    }

                                    std::cout << "\n✨ [System] 기절하여 서버에 의해 마을(X:" << my_x << ", Y:" << my_y << ")로 강제 이동(부활) 되었습니다!\n";

                                    // 키보드를 누르지 않아도 상태창(내 정보)을 즉시 다시 그려줍니다.
                                    std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
                                }
                            }
                        }
                    }
                    // 전투(피격 및 공격 결과) 패킷 처리
                    else if (h.id == Protocol::PKT_GATEWAY_CLIENT_ATTACK_RES) {
                        Protocol::AttackRes attack_res;
                        if (attack_res.ParseFromArray(p.data(), p.size())) {

                            // 1. 내가 공격했는데 허공을 가른 경우 (서버가 damage를 0으로 보냄)
                            if (attack_res.damage() == 0) {
                                std::cout << "\n[System] 범위에 벗어나 공격에 실패했습니다.\n";
                                std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
                            }
                            // 2. 내가 몬스터에게 맞은 경우
                            else if (attack_res.target_account_id() == my_id) {
                                my_hp = attack_res.target_remain_hp();
                                std::cout << "\n🩸 [전투] 몬스터에게 " << attack_res.damage() << " 데미지를 입었습니다!\n";

                                if (my_hp <= 0) {
                                    std::cout << "💀 체력이 0이 되어 기절했습니다...\n";
                                }
                                std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
                            }
                            // 3. 누군가(혹은 내가) 몬스터를 성공적으로 때린 경우
                            else {
                                std::cout << "\n[Combat] ⚔️ 몬스터(" << attack_res.target_account_id()
                                    << ") 타격 성공! 데미지: " << attack_res.damage()
                                    << " (남은 체력: " << attack_res.target_remain_hp() << ")\n";

                                // 몬스터의 남은 체력이 0 이하라면 쓰러졌다는 메시지를 띄웁니다
                                if (attack_res.target_remain_hp() <= 0) {
                                    std::cout << "🎉 [System] 💀 몬스터(" << attack_res.target_account_id() << ")가 쓰러졌습니다!\n";
                                }

                                std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
                            }
                        }
                    }
                }
            }
            catch (...) { std::cout << "\n[서버 연결 종료]\n"; }
            });
        recv_thread.detach();


        // 하나의 세련된 논블로킹 키보드 제어 루프로 통합
        std::cout << "\n [액션 모드] 방향키: 이동 / a키: 공격 / Enter: 채팅 / ESC: 종료\n";
        std::cout << "--------------------------------------\n";

        // [사용자 입력 전용 메인 루프 (액션/채팅 모드 제어)]
        while (true) {
            // 키보드 입력이 있을 때만 반응 (블로킹 되지 않음)
            if (_kbhit()) {
                int key = _getch();

                // 1. 방향키 입력 감지 (특수키는 224가 먼저 들어옵니다)
                if (key == 224) {
                    key = _getch();
                    bool moved = false;
                    switch (key) {
                    case 72: my_y += 1.0f; moved = true; break; // UP
                    case 80: my_y -= 1.0f; moved = true; break; // DOWN
                    case 75: my_x -= 1.0f; moved = true; break; // LEFT
                    case 77: my_x += 1.0f; moved = true; break; // RIGHT
                    }

                    if (moved) {
                        Protocol::MoveReq move_req;
                        move_req.set_x(my_x);
                        move_req.set_y(my_y);
                        move_req.set_z(0.0f);
                        move_req.set_yaw(0.0f);
                        SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_MOVE_REQ, move_req);

                        // 내 위치 옆에 HP 정보도 실시간으로 표시해 줍니다.
                        std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
                    }
                }
                // 2. Enter 키 (13) 누름 -> [채팅 모드] 진입
                else if (key == 13) {
                    std::cout << "\n[채팅 모드] 입력> ";
                    std::string input;
                    // 여기서만 일시적으로 std::getline이 실행되어 타이핑을 받습니다.
                    std::getline(std::cin, input);

                    if (!input.empty()) {
                        Protocol::ChatReq chat_req;
                        chat_req.set_msg(AnsiToUtf8(input));
                        SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_CHAT_REQ, chat_req);
                    }
                    std::cout << "[액션 모드] 방향키: 이동 / a키: 공격 / Enter: 채팅 / ESC: 종료\n";
                }
                // 3. ESC 키 (27) 누름 -> 프로그램 종료
                else if (key == 27) {
                    std::cout << "\n[DummyClient] 접속을 종료합니다.\n";
                    break;
                }
                else if (key == 'a' || key == 'A' || key == 'ㅁ') {
                    Protocol::AttackReq req;
                    req.set_target_uid(0); // 타겟팅은 서버가 자동으로 하므로 0을 보냅니다.

                    SendPacket(socket, Protocol::PKT_CLIENT_GATEWAY_ATTACK_REQ, req);
                }
            }

            // CPU 100% 점유 방지를 위한 짧은 휴식
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        socket.close();
    }
    catch (std::exception& e) { std::cerr << "[Error] " << e.what() << "\n"; }
    return 0;
}