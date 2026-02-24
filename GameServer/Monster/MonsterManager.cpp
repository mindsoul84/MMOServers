#include "MonsterManager.h"
#include "Monster.h"
#include "../Pathfinder/Pathfinder.h"
#include "../Zone/Zone.h"
#include "protocol.pb.h"
#include <iostream>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <boost/asio.hpp> // [추가]

// ==========================================
// GameServer.cpp의 전역 변수들 참조 (extern)
// ==========================================
extern NavMesh g_navMesh;
extern std::vector<std::shared_ptr<Monster>> g_monsters;
extern std::unique_ptr<Zone> g_zone;
//extern std::mutex g_gameMutex;

extern boost::asio::io_context g_io_context;                     // [추가]
extern boost::asio::io_context::strand g_game_strand;            // [추가]

struct PlayerInfo { uint64_t uid; float x, y; };
extern std::unordered_map<std::string, PlayerInfo> g_playerMap;
extern std::unordered_map<uint64_t, std::string> g_uidToAccount;

// [추가] AI 타이머와 상태 저장을 위한 변수
std::unique_ptr<boost::asio::steady_timer> g_ai_timer;
auto g_last_ai_time = std::chrono::steady_clock::now();
std::unordered_map<uint64_t, float> g_sync_timers;
const float NETWORK_SYNC_INTERVAL = 2.0f;

// ==========================================
// [1] 몬스터 초기 스폰 함수
// ==========================================
void InitMonsters() {
    struct SpawnData { float x, y; };
    SpawnData spawn_list[3] = {
        { 5.0f, 45.0f },   // 10000번 몬스터 고향
        { 8.0f, 7.0f },    // 10001번 몬스터 고향
        { 11.0f, 9.0f }    // 10002번 몬스터 고향
    };

    for (int i = 0; i < 3; ++i) {
        uint64_t mon_id = 10000 + i;
        auto mon = std::make_shared<Monster>(mon_id, &g_navMesh);

        mon->SetPosition(spawn_list[i].x, spawn_list[i].y, 0.0f);
        mon->SetSpawnPosition(spawn_list[i].x, spawn_list[i].y, 0.0f);

        g_monsters.push_back(mon);
        g_zone->EnterZone(mon_id, mon->GetPosition().x, mon->GetPosition().y);

        std::cout << "  -> [스폰] 몬스터(ID:" << mon_id << ") 생성됨. 좌표 (X:"
            << mon->GetPosition().x << ", Y:" << mon->GetPosition().y << ")\n";
    }
    std::cout << "[MonsterManager] 몬스터 " << g_monsters.size() << "마리 스폰 완료 및 Zone 등록됨.\n";
}


// [추가] 100ms마다 실행될 재귀적 비동기 타이머 함수
void ScheduleNextAITick() {
    g_ai_timer->expires_after(std::chrono::milliseconds(100));

    // 타이머가 만료되면, 람다 함수 내부의 AI 로직을 Strand 대기열에 던집니다!
    g_ai_timer->async_wait(boost::asio::bind_executor(g_game_strand, [](const boost::system::error_code& ec) {
        if (ec) return;

        auto current_time = std::chrono::steady_clock::now();
        float delta_time = std::chrono::duration<float>(current_time - g_last_ai_time).count();
        g_last_ai_time = current_time;

        // --- 여기서부터는 락(Lock) 없이 안전하게 실행됨 ---
        for (auto& mon : g_monsters) {
            float old_x = mon->GetPosition().x;
            float old_y = mon->GetPosition().y;

            // (기존 AI 로직 : IDLE, CHASE 상태 검사)
            if (mon->GetState() == MonsterState::IDLE) {
                auto aoi_uids = g_zone->GetPlayersInAOI(old_x, old_y);
                for (uint64_t uid : aoi_uids) {
                    if (uid < 10000) {
                        auto it_acc = g_uidToAccount.find(uid);
                        if (it_acc != g_uidToAccount.end()) {
                            auto it_player = g_playerMap.find(it_acc->second);
                            if (it_player != g_playerMap.end()) {
                                float dx = it_player->second.x - old_x;
                                float dy = it_player->second.y - old_y;
                                if (std::sqrt(dx * dx + dy * dy) < 0.1f) { // 시야
                                    mon->SetTarget(uid, { it_player->second.x, it_player->second.y, 0.0f });
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            else if (mon->GetState() == MonsterState::CHASE) {
                uint64_t target_uid = mon->GetTargetUserId();
                bool target_found = false;
                auto it_acc = g_uidToAccount.find(target_uid);
                if (it_acc != g_uidToAccount.end()) {
                    auto it_player = g_playerMap.find(it_acc->second);
                    if (it_player != g_playerMap.end()) {
                        float dx = it_player->second.x - old_x;
                        float dy = it_player->second.y - old_y;
                        if (std::sqrt(dx * dx + dy * dy) <= 3.0f) {
                            mon->UpdateTargetPosition({ it_player->second.x, it_player->second.y, 0.0f });
                            target_found = true;
                        }
                    }
                }
                if (!target_found) mon->GiveUpChase();
            }

            mon->Update(delta_time);

            float new_x = mon->GetPosition().x;
            float new_y = mon->GetPosition().y;

            if (std::abs(old_x - new_x) > 0.05f || std::abs(old_y - new_y) > 0.05f) {
                g_zone->UpdatePosition(mon->GetId(), old_x, old_y, new_x, new_y);
                g_sync_timers[mon->GetId()] += delta_time;

                if (g_sync_timers[mon->GetId()] >= NETWORK_SYNC_INTERVAL) {
                    g_sync_timers[mon->GetId()] = 0.0f;
                    auto aoi_uids = g_zone->GetPlayersInAOI(new_x, new_y);

                    if (!aoi_uids.empty()) {
                        Protocol::GameGatewayMoveRes s2s_res;
                        s2s_res.set_account_id("MONSTER_" + std::to_string(mon->GetId()));
                        s2s_res.set_x(new_x);
                        s2s_res.set_y(new_y);
                        s2s_res.set_z(mon->GetPosition().z);
                        s2s_res.set_yaw(0.0f);

                        for (uint64_t target_uid : aoi_uids) {
                            auto it = g_uidToAccount.find(target_uid);
                            if (it != g_uidToAccount.end()) {
                                s2s_res.add_target_account_ids(it->second);
                            }
                        }
                        // (GateWay 전송 로직이 들어갈 자리)
                    }
                }
            }
        }

        // 1번의 틱이 끝나면, 다음 100ms 뒤를 다시 예약합니다. (무한 반복 루프 완성)
        ScheduleNextAITick();
    }));
}

// =================================================================
// [2] AI 메인 게임 루프 (이제 Thread가 아니라 Timer를 가동합니다!)
// =================================================================
void StartAITickThread() {
    g_ai_timer = std::make_unique<boost::asio::steady_timer>(g_io_context);
    g_last_ai_time = std::chrono::steady_clock::now();

    // 첫 번째 틱을 스케줄링하여 무한 궤도에 진입시킵니다.
    ScheduleNextAITick();
    std::cout << "[MonsterManager] 비동기 Strand 기반 AI 타이머 루프 가동 시작 (10 FPS)\n";
}