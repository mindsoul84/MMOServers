#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <boost/asio.hpp>
#include <windows.h> // Windows API Header
#include <unordered_map>
#include <mutex>

#include <fstream>      
#include <cstring>      // memset 사용을 위해 추가
#include <recastnavigation/DetourNavMesh.h>         // 핵심 구조체 정의 포함
#include <recastnavigation/DetourNavMeshBuilder.h>

#include <boost/asio/strand.hpp>

#include "../Common/ConfigManager.h"
#include "../Common/DBManager.h"

#include "protocol.pb.h"
#include "PacketDispatcher.h"

// 기존 작성한 게임 로직 헤더들은 이 자리 그대로 유지
#include "Zone/Zone.h"
#include "Monster/Monster.h"
#include "Pathfinder/Pathfinder.h"
#include "Pathfinder/MapGenerator.h"
#include "Monster/MonsterManager.h"


#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

using boost::asio::ip::tcp;

// ==========================================
// ★ 게임 월드 상태 및 Zone 관리
// ==========================================
//Zone g_zone(1000, 1000, 50); // 예: 1000x1000 맵, 격자 크기 50
std::unique_ptr<Zone> g_zone;

// ★ AI 연동을 위한 전역 변수
NavMesh g_navMesh;
std::vector<std::shared_ptr<Monster>> g_monsters;

struct PlayerInfo {
    uint64_t uid;
    float x, y;
    int hp = 100;
};

std::unordered_map<std::string, PlayerInfo> g_playerMap;  // account_id -> PlayerInfo
std::unordered_map<uint64_t, std::string> g_uidToAccount; // uid -> account_id
uint64_t g_uidCounter = 1; // Zone에 넣을 고유 번호 발급용

//std::mutex g_gameMutex;
// ★ [핵심 추가] 게임 로직을 순차적으로 처리할 글로벌 IO 컨텍스트와 Strand(작업 대기열)
boost::asio::io_context g_io_context;
boost::asio::io_context::strand g_game_strand(g_io_context);

// ==========================================
// 1. 전역 변수 및 디스패처 설정
// ==========================================
class GatewaySession;
PacketDispatcher<GatewaySession> g_s2s_gateway_dispatcher;

// 게이트웨이 접속 개수 카운터 (일반적으로 1개지만 확장성을 위해 유지)
static std::atomic<int> g_connected_gateways{ 0 };

// ★ [추가] 연결된 게이트웨이 세션들을 보관할 리스트 (AI 스레드에서 접근하기 위함)
std::vector<std::shared_ptr<GatewaySession>> g_gatewaySessions;
std::mutex g_gatewaySessionMutex;

// ==========================================
// 2. GatewaySession: Gateway로부터의 S2S 통신(수신/송신) 담당
// ==========================================
class GatewaySession : public std::enable_shared_from_this<GatewaySession> {
private:
    tcp::socket socket_;
    PacketHeader header_;
    std::vector<char> payload_buf_;

public:
    GatewaySession(tcp::socket socket) noexcept : socket_(std::move(socket)) {}

    void start() {
        ReadHeader();
    }

    void Send(uint16_t pktId, const google::protobuf::Message& msg) {
        std::string payload;
        msg.SerializeToString(&payload);
        PacketHeader header;
        header.size = static_cast<uint16_t>(sizeof(PacketHeader) + payload.size());
        header.id = pktId;

        auto send_buf = std::make_shared<std::vector<char>>(header.size);
        memcpy(send_buf->data(), &header, sizeof(PacketHeader));
        memcpy(send_buf->data() + sizeof(PacketHeader), payload.data(), payload.size());

        auto self(shared_from_this());
        boost::asio::async_write(socket_, boost::asio::buffer(*send_buf),
            [this, self, send_buf](boost::system::error_code ec, std::size_t) {
                if (ec) std::cerr << "[GameServer] Gateway로 S2S 패킷 전송 실패\n";
            });
    }

private:
    void ReadHeader() {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(&header_, sizeof(PacketHeader)),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    if (header_.size < sizeof(PacketHeader) || header_.size > 4096) return;
                    uint16_t payload_size = static_cast<uint16_t>(header_.size - sizeof(PacketHeader));

                    if (payload_size == 0) {
                        // ★ 잠재적 에러 사전 차단: self의 복사본을 만들어 넘겨줍니다.
                        auto session_ptr = self;
                        g_s2s_gateway_dispatcher.Dispatch(session_ptr, header_.id, nullptr, 0);
                        ReadHeader();
                    }
                    else {
                        payload_buf_.resize(payload_size);
                        ReadPayload(payload_size);
                    }
                }
                else {
                    int current_count = --g_connected_gateways;
                    std::cout << "[GameServer] GatewayServer와의 S2S 연결 해제됨. (현재 연결된 Gateway: " << current_count << "개)\n";
                }
            });
    }

    void ReadPayload(uint16_t payload_size) {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(payload_buf_.data(), payload_size),
            [this, self, payload_size](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    // ★ 잠재적 에러 사전 차단: 복사본을 만들어 넘겨줍니다.
                    auto session_ptr = self;
                    g_s2s_gateway_dispatcher.Dispatch(session_ptr, header_.id, payload_buf_.data(), payload_size);
                    ReadHeader();
                }
            });
    }
};

// 모든 게이트웨이에 패킷을 뿌려주는 전역 브로드캐스트 함수
void BroadcastToGateways(uint16_t pktId, const google::protobuf::Message& msg)
{
    std::lock_guard<std::mutex> lock(g_gatewaySessionMutex);
    for (auto& session : g_gatewaySessions) {
        if (session) session->Send(pktId, msg);
    }
}

// ==========================================
// 이동 패킷 핸들러 (Strand 적용)
// ==========================================
void Handle_GatewayGameMoveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    // 1. [병렬 처리 구간] I/O 스레드들이 락 없이 각자 파싱만 빠르게 수행합니다.
    auto req = std::make_shared<Protocol::GatewayGameMoveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) return;

    // 2. [직렬 큐잉 구간] 파싱된 데이터를 Strand 대기열에 던집니다. (람다 캡처 주의!)
    boost::asio::post(g_game_strand, [session, req]() {
        // --- 여기서부터는 무조건 순차 실행이 보장되므로 락(Lock)이 전혀 필요 없습니다! ---
        std::string acc_id = req->account_id();
        float new_x = req->x();
        float new_y = req->y();

        std::vector<uint64_t> aoi_uids;

        // 1. 처음 이동하는(입장한) 유저라면 Zone에 등록
        if (g_playerMap.find(acc_id) == g_playerMap.end()) {
            uint64_t new_uid = g_uidCounter++;
            g_playerMap[acc_id] = { new_uid, new_x, new_y };
            g_uidToAccount[new_uid] = acc_id;
            g_zone->EnterZone(new_uid, new_x, new_y);
            std::cout << "[GameServer] 유저(" << acc_id << ") 최초 Zone 진입 (UID:" << new_uid << ")\n";
        }
        // 2. 이미 있는 유저라면 좌표 Update
        else {
            auto& info = g_playerMap[acc_id];
            g_zone->UpdatePosition(info.uid, info.x, info.y, new_x, new_y);
            info.x = new_x;
            info.y = new_y;
        }

        // 3. 내 주변(AOI)에 있는 유저 목록 추출
        aoi_uids = g_zone->GetPlayersInAOI(new_x, new_y);


        // 4. Gateway에게 "이 유저들에게만 뿌려!" 라고 패킷 전송
        Protocol::GameGatewayMoveRes s2s_res;
        s2s_res.set_account_id(acc_id);
        s2s_res.set_x(new_x);
        s2s_res.set_y(new_y);
        s2s_res.set_z(req->z());
        s2s_res.set_yaw(req->yaw());

        // [수정] 유저와 몬스터 숫자를 따로 세기 위한 변수
        int user_count = 0;
        int monster_count = 0;

        for (uint64_t target_uid : aoi_uids) {
            // 10000 미만은 유저, 이상은 몬스터로 분류하여 카운팅
            if (target_uid < 10000) {
                user_count++;

                // 유저인 경우에만 패킷 수신 대상(target_account_ids)에 넣습니다.
                auto it = g_uidToAccount.find(target_uid);
                if (it != g_uidToAccount.end()) {
                    s2s_res.add_target_account_ids(it->second);
                }
            }
            else {
                monster_count++;
            }
        }

        // Gateway로 전달
        session->Send(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
        // [수정] 뭉뚱그려진 로그를 유저와 몬스터로 분리해서 출력합니다.
        std::cout << "[GameServer] 유저(" << acc_id << ") 이동 완료 -> AOI 수신 대상: 유저 " << user_count << "명, 몬스터 " << monster_count << "마리\n";
    });
}

// ==========================================
// [GameServer] 유저 퇴장 처리 핸들러(Strand 적용)
// ==========================================
void Handle_GatewayGameLeaveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::GatewayGameLeaveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) return;

    boost::asio::post(g_game_strand, [req]() {
        // --- 락(Lock) 없이 안전하게 순차 삭제 ---
        std::string acc_id = req->account_id();

        // 1. 해당 유저가 GameServer에 존재하는지 확인
        auto it = g_playerMap.find(acc_id);
        if (it != g_playerMap.end()) {
            uint64_t uid = it->second.uid;
            float last_x = it->second.x;
            float last_y = it->second.y;

            // 2. Zone(공간)에서 유저의 물리적 실체 삭제
            g_zone->LeaveZone(uid, last_x, last_y);

            // 3. GameServer의 관리 맵에서 완전히 삭제
            g_uidToAccount.erase(uid);
            g_playerMap.erase(it);

            std::cout << "[GameServer] 👻 유저(" << acc_id << ", UID:" << uid << ") 퇴장 완료. Zone에서 유령 데이터 삭제됨.\n";
        }
    });
}

// ==========================================
// 3. GameNetworkServer: 9000번 포트에서 Gateway의 접속(Accept) 대기
// ==========================================
class GameNetworkServer {
    tcp::acceptor acceptor_;
public:
    GameNetworkServer(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }
private:
    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                int current_count = ++g_connected_gateways;
                std::cout << "[GameServer] S2S 통신: GatewayServer 접속 확인 완료! (현재 연결된 Gateway: " << current_count << "개)\n";

                // =========================================================
                // ★ [수정] 세션을 즉시 실행만 하는 것이 아니라, 
                // 나중에 BroadcastToGateways에서 쓸 수 있도록 전역 리스트에 꼭 넣어주어야 합니다!
                // =========================================================
                auto new_session = std::make_shared<GatewaySession>(std::move(socket));
                {
                    std::lock_guard<std::mutex> lock(g_gatewaySessionMutex);
                    g_gatewaySessions.push_back(new_session);
                }

                new_session->start();
                // =========================================================
            }
            else {
                std::cerr << "[Error] Gateway Accept 실패: " << ec.message() << "\n";
            }
            do_accept();
            });
    }
};

// ==========================================
// 4. 메인 함수: 스레드 풀 구성 및 서버 실행
// ==========================================
int main() {
    // 윈도우 콘솔 한글 깨짐 방지
    SetConsoleOutputCP(CP_UTF8);

    // 1. 가장 먼저 환경 설정(config.json)을 로드합니다.
    ConfigManager::GetInstance().LoadConfig("config.json");

    // 2. 설정에 DB 연동이 true로 되어 있다면 DB 연결 시도
    if (ConfigManager::GetInstance().UseDB()) {
        if (!DBManager::GetInstance().Connect()) {
            std::cerr << "DB 연결에 실패하여 서버를 종료합니다.\n";
            return -1;
        }
    }
    else {
        std::cout << "[System] ⚠️ config.json 설정에 따라 DB 연동을 건너뜁니다.\n";
    }

    // 추가: 한글 세팅이 끝난 안전한 타이밍에 Zone을 생성합니다!
    g_zone = std::make_unique<Zone>(1000, 1000, 50);

    // [추가] 파일이 없으면 즉석에서 만들어주는 제너레이터 가동!
    GenerateDummyMapFile("dummy_map.bin");

    // ---------------------------------------------------------
    // 맵 데이터 로드 및 몬스터 스폰
    // ---------------------------------------------------------
    g_navMesh.LoadNavMeshFromFile("dummy_map.bin");

    // 3. 몬스터 스폰 및 AI 시스템 가동 (분리된 모듈 호출)
    InitMonsters();
    StartAITickThread();
    
    // ★ 디스패처 등록
    g_s2s_gateway_dispatcher.RegisterHandler(Protocol::PKT_GATEWAY_GAME_MOVE_REQ, Handle_GatewayGameMoveReq);
    g_s2s_gateway_dispatcher.RegisterHandler(Protocol::PKT_GATEWAY_GAME_LEAVE_REQ, Handle_GatewayGameLeaveReq);
    
    try {
        //boost::asio::io_context io_context;

        // 1. S2S 서버 객체 생성 (포트: 9000)
        GameNetworkServer server(g_io_context, 9000);
        std::cout << "[System] 코어 게임 로직 서버 가동 (Port: 9000) Created by Jeong Shin Young\n";

        // 2. CPU 코어 개수에 맞춰 스레드 개수 설정
        unsigned int thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        std::cout << "[System] 워커 스레드 개수 설정: " << thread_count << "개\n";

        // 3. 스레드 풀 생성 및 io_context.run() 실행
        std::cout << "[System] 여러 스레드에서 io_context.run()을 호출하여 스레드 풀을 구성합니다...\n";
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < thread_count; ++i) {
            threads.emplace_back([]() {
                g_io_context.run();
            });
        }
        std::cout << "[System] 스레드 풀 구성 완료.\n";

        // 4. 메인 스레드 대기 (join)
        std::cout << "=================================================\n";
        std::cout << "[System] GatewayServer의 S2S 접속을 기다리는 중...\n";

        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    catch (std::exception& e) {
        std::cerr << "[Error] 예외 발생: " << e.what() << "\n";
    }

    std::cout << "[System] 서버가 안전하게 종료되었습니다.\n";
    return 0;
}