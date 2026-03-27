#include "MonsterManager.h"
#include "../Common/DataManager/MonsterDataManager.h"
#include "Monster.h"
#include "../Pathfinder/Pathfinder.h"
#include "../Zone/Zone.h"

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include "../GameServer.h"
#include "../../Common/Redis/RedisManager.h"

#include <iostream>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <cmath>
#include <boost/asio.hpp>

static std::unique_ptr<boost::asio::steady_timer> g_ai_timer;
static auto g_last_ai_time = std::chrono::steady_clock::now();
static std::unordered_map<uint64_t, float> g_sync_timers;
static const float NETWORK_SYNC_INTERVAL = 2.0f;

// ==========================================
// 몬스터 초기 스폰 함수
// ==========================================
void InitMonsters() {
    auto& ctx = GameContext::Get();

    const auto& spawnList = ctx.dataManager.GetMonsterData().GetMonsterSpawnList();

    for (const auto& spawn_data : spawnList) {
        uint64_t mon_id = spawn_data.mon_id;
        auto mon = std::make_shared<Monster>(mon_id, &ctx.navMesh);

        mon->SetOnAttackCallback([](uint64_t attacker_uid, uint64_t target_uid, int damage) {
            auto& ctx_inner = GameContext::Get();
            std::shared_ptr<PlayerInfo> player_ptr;
            std::string acc_id_str;

            // playerMutex_ 읽기 락: 유저 찾기
            {
                std::shared_lock<std::shared_mutex> read_lock(ctx_inner.playerMutex_);
                auto it_acc = ctx_inner.uidToAccount.find(target_uid);
                if (it_acc == ctx_inner.uidToAccount.end()) return;
                acc_id_str = it_acc->second;

                auto it_player = ctx_inner.playerMap.find(it_acc->second);
                if (it_player == ctx_inner.playerMap.end()) return;
                player_ptr = it_player->second;
            }

            int remain_hp = 0;
            float p_x, p_y;

            // 개별 유저 락으로 체력 깎기
            {
                std::lock_guard<std::mutex> p_lock(player_ptr->mtx);
                player_ptr->hp -= damage;
                if (player_ptr->hp < 0) player_ptr->hp = 0;
                remain_hp = player_ptr->hp;
                p_x = player_ptr->x;
                p_y = player_ptr->y;
            }

            RedisManager::GetInstance().UpdatePlayerHp(acc_id_str, remain_hp);

            Protocol::GameGatewayAttackRes s2s_res;
            s2s_res.set_attacker_uid(attacker_uid);
            s2s_res.set_target_uid(target_uid);
            s2s_res.set_target_account_id(acc_id_str);
            s2s_res.set_damage(damage);
            s2s_res.set_target_remain_hp(remain_hp);

            auto aoi_uids = ctx_inner.zone->GetPlayersInAOI(p_x, p_y);

            // playerMutex_ 읽기 락: AOI 유저 이름 조회
            {
                std::shared_lock<std::shared_mutex> read_lock(ctx_inner.playerMutex_);
                for (uint64_t aoi_uid : aoi_uids) {
                    auto target_acc = ctx_inner.uidToAccount.find(aoi_uid);
                    if (target_acc != ctx_inner.uidToAccount.end()) {
                        s2s_res.add_target_account_ids(target_acc->second);
                    }
                }
            }

            ctx_inner.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, s2s_res);

            std::cout << "[Combat] 몬스터(" << attacker_uid << ")가 유저(" << acc_id_str
                << ")를 공격! 데미지: " << damage << ", 남은 체력: " << remain_hp << "\n";

            // =====================================================================
            // 유저 기절 시 마을로 텔레포트 처리
            // =====================================================================
            if (remain_hp <= 0) {
                std::cout << "\n[System] 당신(" << acc_id_str << ")은 체력이 0이되어 기절했습니다. 마을(X:0, Y:0)로 복귀합니다.\n\n";

                float old_x = p_x;
                float old_y = p_y;

                {
                    std::lock_guard<std::mutex> p_lock(player_ptr->mtx);
                    player_ptr->hp = 100;
                    player_ptr->x = 0.0f;
                    player_ptr->y = 0.0f;
                }

                // Redis HP 갱신 (부활: HP 100으로 복구)
                RedisManager::GetInstance().UpdatePlayerHp(acc_id_str, 100);

                ctx_inner.zone->UpdatePosition(target_uid, old_x, old_y, 0.0f, 0.0f);

                Protocol::GameGatewayMoveRes teleport_res;
                teleport_res.set_account_id(acc_id_str);
                teleport_res.set_x(0.0f);
                teleport_res.set_y(0.0f);
                teleport_res.set_z(0.0f);
                teleport_res.set_yaw(0.0f);
                teleport_res.add_target_account_ids(acc_id_str);

                ctx_inner.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_MOVE_RES, teleport_res);
                return;
            }
        });

        // JSON에서 읽어온 체력과 리스폰 시간을 몬스터에게 부여
        mon->SetMaxHp(spawn_data.hp);
        mon->SetRespawnSec(spawn_data.respawn_sec);

        mon->SetPosition(spawn_data.x, spawn_data.y, 0.0f);
        mon->SetSpawnPosition(spawn_data.x, spawn_data.y, 0.0f);

        ctx.monsters.push_back(mon);
        ctx.monsterMap[mon_id] = mon;
        ctx.zone->EnterZoneMonster(mon_id, mon->GetPosition().x, mon->GetPosition().y);

        std::cout << "  -> [스폰] 몬스터(ID:" << mon_id << ", HP:" << spawn_data.hp << ", 리스폰:" << spawn_data.respawn_sec << "초) 생성됨." <<
                     " 좌표 (X:" << mon->GetPosition().x << ", Y:" << mon->GetPosition().y << ")\n";
    }
    std::cout << "[MonsterManager] 몬스터 " << ctx.monsters.size() << "마리 스폰 완료 및 Zone 등록됨.\n";
}

void ScheduleNextAITick() {
    g_ai_timer->expires_after(std::chrono::milliseconds(100));

    g_ai_timer->async_wait([](const boost::system::error_code& ec) {
        if (ec) return;

        auto& ctx = GameContext::Get();

        auto current_time = std::chrono::steady_clock::now();
        float delta_time = std::chrono::duration<float>(current_time - g_last_ai_time).count();
        g_last_ai_time = current_time;

        // monsterMutex_ 읽기 락: 몬스터 상태 읽기 (playerMutex_와 독립)
        std::shared_lock<std::shared_mutex> mon_lock(ctx.monsterMutex_);

        for (auto& mon : ctx.monsters) {

            // 몬스터가 죽어있다면 타이머를 돌리고 부활
            if (mon->GetState() == MonsterState::DEAD) {
                mon->AddDeadTime(delta_time);

                if (mon->GetDeadTime() >= mon->GetRespawnSec()) {
                    mon->Respawn();

                    std::cout << "[System] 몬스터(ID:" << mon->GetId() << ")가 " << mon->GetRespawnSec() << "초가 지나서 리스폰되었습니다!\n";

                    // 부활한 사실을 주변 유저들에게 알려서 화면에 다시 나타나게 합니다.
                    auto aoi_uids = ctx.zone->GetPlayersInAOI(mon->GetPosition().x, mon->GetPosition().y);
                    if (!aoi_uids.empty()) {
                        Protocol::GameGatewayMoveRes s2s_res;
                        s2s_res.set_account_id("MONSTER_" + std::to_string(mon->GetId()));
                        s2s_res.set_x(mon->GetPosition().x);
                        s2s_res.set_y(mon->GetPosition().y);
                        s2s_res.set_z(mon->GetPosition().z);
                        s2s_res.set_yaw(0.0f);

                        // playerMutex_ 읽기 락: uidToAccount 조회
                        // 락 순서: monsterMutex_(이미 보유) -> playerMutex_ (항상 이 순서로 획득)
                        {
                            std::shared_lock<std::shared_mutex> p_lock(ctx.playerMutex_);
                            for (uint64_t target_uid : aoi_uids) {
                                auto it = ctx.uidToAccount.find(target_uid);
                                if (it != ctx.uidToAccount.end()) {
                                    s2s_res.add_target_account_ids(it->second);
                                }
                            }
                        }
                        ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
                    }
                }

                continue;
            }

            float old_x = mon->GetPosition().x;
            float old_y = mon->GetPosition().y;

            if (mon->GetState() == MonsterState::IDLE) {
                auto aoi_uids = ctx.zone->GetPlayersInAOI(old_x, old_y);

                // playerMutex_ 읽기 락: 유저 위치 조회
                {
                    std::shared_lock<std::shared_mutex> p_lock(ctx.playerMutex_);
                    for (uint64_t uid : aoi_uids) {
                        auto it_acc = ctx.uidToAccount.find(uid);
                        if (it_acc != ctx.uidToAccount.end()) {
                            auto it_player = ctx.playerMap.find(it_acc->second);
                            if (it_player != ctx.playerMap.end()) {
                                float dx = it_player->second->x - old_x;
                                float dy = it_player->second->y - old_y;
                                if (std::sqrt(dx * dx + dy * dy) < 1.0f) {
                                    mon->SetTarget(uid, { it_player->second->x, it_player->second->y, 0.0f });
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

                {
                    std::shared_lock<std::shared_mutex> p_lock(ctx.playerMutex_);
                    auto it_acc = ctx.uidToAccount.find(target_uid);
                    if (it_acc != ctx.uidToAccount.end()) {
                        auto it_player = ctx.playerMap.find(it_acc->second);
                        if (it_player != ctx.playerMap.end()) {
                            float dx = it_player->second->x - old_x;
                            float dy = it_player->second->y - old_y;
                            if (std::sqrt(dx * dx + dy * dy) <= 3.0f) {
                                mon->UpdateTargetPosition({ it_player->second->x, it_player->second->y, 0.0f });
                                target_found = true;
                            }
                        }
                    }
                }
                if (!target_found) mon->GiveUpChase();
            }
            else if (mon->GetState() == MonsterState::ATTACK) {
                uint64_t target_uid = mon->GetTargetUserId();
                bool target_found = false;

                {
                    std::shared_lock<std::shared_mutex> p_lock(ctx.playerMutex_);
                    auto it_acc = ctx.uidToAccount.find(target_uid);
                    if (it_acc != ctx.uidToAccount.end()) {
                        auto it_player = ctx.playerMap.find(it_acc->second);
                        if (it_player != ctx.playerMap.end()) {
                            float dx = it_player->second->x - old_x;
                            float dy = it_player->second->y - old_y;
                            if (std::sqrt(dx * dx + dy * dy) <= 3.0f) {
                                mon->UpdateTargetPosition({ it_player->second->x, it_player->second->y, 0.0f });
                                target_found = true;
                            }
                        }
                    }
                }
                if (!target_found) mon->GiveUpAttack();
            }

            mon->Update(delta_time);

            float new_x = mon->GetPosition().x;
            float new_y = mon->GetPosition().y;

            if (std::abs(old_x - new_x) > 0.05f || std::abs(old_y - new_y) > 0.05f) {
                ctx.zone->UpdatePositionMonster(mon->GetId(), old_x, old_y, new_x, new_y);
                g_sync_timers[mon->GetId()] += delta_time;

                if (g_sync_timers[mon->GetId()] >= NETWORK_SYNC_INTERVAL) {
                    g_sync_timers[mon->GetId()] = 0.0f;
                    auto aoi_uids = ctx.zone->GetPlayersInAOI(new_x, new_y);

                    if (!aoi_uids.empty()) {
                        Protocol::GameGatewayMoveRes s2s_res;
                        s2s_res.set_account_id("MONSTER_" + std::to_string(mon->GetId()));
                        s2s_res.set_x(new_x);
                        s2s_res.set_y(new_y);
                        s2s_res.set_z(mon->GetPosition().z);
                        s2s_res.set_yaw(0.0f);

                        {
                            std::shared_lock<std::shared_mutex> p_lock(ctx.playerMutex_);
                            for (uint64_t target_uid : aoi_uids) {
                                auto it = ctx.uidToAccount.find(target_uid);
                                if (it != ctx.uidToAccount.end()) {
                                    s2s_res.add_target_account_ids(it->second);
                                }
                            }
                        }
                        ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
                    }
                }
            }
        }

        ScheduleNextAITick();
        });
}

void StartAITickThread() {
    auto& ctx = GameContext::Get();
    g_ai_timer = std::make_unique<boost::asio::steady_timer>(ctx.io_context);
    g_last_ai_time = std::chrono::steady_clock::now();

    ScheduleNextAITick();
    std::cout << "[MonsterManager] 비동기 Strand 기반 AI 타이머 루프 가동 시작 (10 FPS)\n";
}
