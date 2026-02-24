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

// ==========================================
// GameServer.cpp의 전역 변수들 참조 (extern)
// ==========================================
extern NavMesh g_navMesh;
extern std::vector<std::shared_ptr<Monster>> g_monsters;
extern std::unique_ptr<Zone> g_zone;
extern std::mutex g_gameMutex;

struct PlayerInfo { uint64_t uid; float x, y; };
extern std::unordered_map<std::string, PlayerInfo> g_playerMap;
extern std::unordered_map<uint64_t, std::string> g_uidToAccount;

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

// ==========================================
// [2] AI 메인 게임 루프 (Tick Thread)
// ==========================================
void StartAITickThread() {
    std::thread ai_tick_thread([]() {
        auto last_time = std::chrono::steady_clock::now();
        const float NETWORK_SYNC_INTERVAL = 2.0f;
        std::unordered_map<uint64_t, float> sync_timers;

        while (true) {
            auto current_time = std::chrono::steady_clock::now();
            float delta_time = std::chrono::duration<float>(current_time - last_time).count();
            last_time = current_time;

            {
                std::lock_guard<std::mutex> lock(g_gameMutex);

                for (auto& mon : g_monsters) {
                    float old_x = mon->GetPosition().x;
                    float old_y = mon->GetPosition().y;

                    // 1. IDLE 상태: 어그로 레이더
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

                                        if (std::sqrt(dx * dx + dy * dy) < 0.1f) {
                                            Vector3 target_pos = { it_player->second.x, it_player->second.y, 0.0f };
                                            mon->SetTarget(uid, target_pos);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // 2. CHASE 상태: 거리 기반 추적 포기 체크
                    else if (mon->GetState() == MonsterState::CHASE) {
                        uint64_t target_uid = mon->GetTargetUserId();
                        bool target_found = false;

                        auto it_acc = g_uidToAccount.find(target_uid);
                        if (it_acc != g_uidToAccount.end()) {
                            auto it_player = g_playerMap.find(it_acc->second);
                            if (it_player != g_playerMap.end()) {
                                float user_x = it_player->second.x;
                                float user_y = it_player->second.y;

                                float dx = user_x - old_x;
                                float dy = user_y - old_y;
                                float distance = std::sqrt(dx * dx + dy * dy);

                                if (distance <= 5.0f) {
                                    mon->UpdateTargetPosition({ user_x, user_y, 0.0f });
                                    target_found = true;
                                }
                            }
                        }

                        if (!target_found) {
                            mon->GiveUpChase();
                        }
                    }

                    // 3. AI 두뇌 가동 (이동 연산)
                    mon->Update(delta_time);

                    // 4. 위치 변화에 따른 Zone 업데이트 및 브로드캐스팅
                    float new_x = mon->GetPosition().x;
                    float new_y = mon->GetPosition().y;

                    if (std::abs(old_x - new_x) > 0.05f || std::abs(old_y - new_y) > 0.05f) {
                        g_zone->UpdatePosition(mon->GetId(), old_x, old_y, new_x, new_y);
                        sync_timers[mon->GetId()] += delta_time;

                        if (sync_timers[mon->GetId()] >= NETWORK_SYNC_INTERVAL) {
                            sync_timers[mon->GetId()] = 0.0f;
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
                                // (실제 Gateway 송신 로직 연동 자리)
                            }
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        });
    ai_tick_thread.detach();
    std::cout << "[MonsterManager] AI Tick 스레드 가동 시작 (10 FPS)\n";
}