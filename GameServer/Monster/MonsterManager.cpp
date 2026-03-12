#include "MonsterManager.h"
#include "../Common/DataManager/MonsterDataManager.h"
#include "Monster.h"
#include "../Pathfinder/Pathfinder.h"
#include "../Zone/Zone.h"
#include "protocol.pb.h"
#include "../GameServer.h" // ★ [핵심] GameContext 접근용

#include <iostream>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <boost/asio.hpp>

// ==========================================
// [삭제됨] 외부 전역 변수들(extern) 및 PlayerInfo 구조체 중복 선언 삭제 
// (모두 GameServer.h 의 GameContext 로 통합되었습니다)
// ==========================================

// 타이머와 상태 저장을 위한 변수 (이 파일에서만 쓰이므로 static으로 보호)
static std::unique_ptr<boost::asio::steady_timer> g_ai_timer;
static auto g_last_ai_time = std::chrono::steady_clock::now();
static std::unordered_map<uint64_t, float> g_sync_timers;
static const float NETWORK_SYNC_INTERVAL = 2.0f;

// ==========================================
// 몬스터 초기 스폰 함수
// ==========================================
void InitMonsters() {
    auto& ctx = GameContext::Get(); // ★ 컨텍스트 소환
    const auto& spawnList = MonsterDataManager::GetInstance().GetMonsterSpawnList();

    for (const auto& spawn_data : spawnList) {
        uint64_t mon_id = spawn_data.mon_id;
        // g_navMesh -> ctx.navMesh 로 변경
        auto mon = std::make_shared<Monster>(mon_id, &ctx.navMesh);

        mon->SetOnAttackCallback([](uint64_t attacker_uid, uint64_t target_uid, int damage) {
            auto& ctx_inner = GameContext::Get(); // ★ 콜백 내부용 컨텍스트 소환

            auto it_acc = ctx_inner.uidToAccount.find(target_uid);
            if (it_acc == ctx_inner.uidToAccount.end()) return;

            auto it_player = ctx_inner.playerMap.find(it_acc->second);
            if (it_player == ctx_inner.playerMap.end()) return;

            it_player->second.hp -= damage;
            if (it_player->second.hp < 0) it_player->second.hp = 0;

            if (it_player->second.hp <= 0) {
                std::cout << "\n[System] 💀 당신(" << it_acc->second << ")은 체력이 0이되어 기절했습니다. 마을(X:0, Y:0)로 복귀합니다.\n\n";

                float old_x = it_player->second.x;
                float old_y = it_player->second.y;

                it_player->second.hp = 100;
                it_player->second.x = 0.0f;
                it_player->second.y = 0.0f;

                ctx_inner.zone->UpdatePosition(target_uid, old_x, old_y, 0.0f, 0.0f);

                Protocol::GameGatewayMoveRes teleport_res;
                teleport_res.set_account_id(it_acc->second);
                teleport_res.set_x(0.0f);
                teleport_res.set_y(0.0f);
                teleport_res.set_z(0.0f);
                teleport_res.set_yaw(0.0f);
                teleport_res.add_target_account_ids(it_acc->second);

                ctx_inner.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_MOVE_RES, teleport_res);
                return;
            }

            std::cout << "[Combat] 💥 몬스터(" << attacker_uid << ")가 유저(" << it_acc->second
                << ")를 공격! 데미지: " << damage << ", 남은 체력: " << it_player->second.hp << "\n";

            Protocol::GameGatewayAttackRes s2s_res;
            s2s_res.set_attacker_uid(attacker_uid);
            s2s_res.set_target_uid(target_uid);
            s2s_res.set_target_account_id(it_acc->second);
            s2s_res.set_damage(damage);
            s2s_res.set_target_remain_hp(it_player->second.hp);

            auto aoi_uids = ctx_inner.zone->GetPlayersInAOI(it_player->second.x, it_player->second.y);
            for (uint64_t aoi_uid : aoi_uids) {
                if (aoi_uid < 10000) {
                    auto target_acc = ctx_inner.uidToAccount.find(aoi_uid);
                    if (target_acc != ctx_inner.uidToAccount.end()) {
                        s2s_res.add_target_account_ids(target_acc->second);
                    }
                }
            }

            ctx_inner.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, s2s_res);
            });

        mon->SetPosition(spawn_data.x, spawn_data.y, 0.0f);
        mon->SetSpawnPosition(spawn_data.x, spawn_data.y, 0.0f);

        ctx.monsters.push_back(mon);
        ctx.zone->EnterZone(mon_id, mon->GetPosition().x, mon->GetPosition().y);

        std::cout << "  -> [스폰] 몬스터(ID:" << mon_id << ") 생성됨. 좌표 (X:"
            << mon->GetPosition().x << ", Y:" << mon->GetPosition().y << ")\n";
    }
    std::cout << "[MonsterManager] 몬스터 " << ctx.monsters.size() << "마리 스폰 완료 및 Zone 등록됨.\n";
}

void ScheduleNextAITick() {
    g_ai_timer->expires_after(std::chrono::milliseconds(100));

    // ★ g_game_strand -> ctx.game_strand 로 변경
    g_ai_timer->async_wait(boost::asio::bind_executor(GameContext::Get().game_strand, [](const boost::system::error_code& ec) {
        if (ec) return;

        auto& ctx = GameContext::Get(); // ★ 타이머 콜백 내부용 컨텍스트 소환

        auto current_time = std::chrono::steady_clock::now();
        float delta_time = std::chrono::duration<float>(current_time - g_last_ai_time).count();
        g_last_ai_time = current_time;

        for (auto& mon : ctx.monsters) {
            float old_x = mon->GetPosition().x;
            float old_y = mon->GetPosition().y;

            if (mon->GetState() == MonsterState::IDLE) {
                auto aoi_uids = ctx.zone->GetPlayersInAOI(old_x, old_y);
                for (uint64_t uid : aoi_uids) {
                    if (uid < 10000) {
                        auto it_acc = ctx.uidToAccount.find(uid);
                        if (it_acc != ctx.uidToAccount.end()) {
                            auto it_player = ctx.playerMap.find(it_acc->second);
                            if (it_player != ctx.playerMap.end()) {
                                float dx = it_player->second.x - old_x;
                                float dy = it_player->second.y - old_y;
                                if (std::sqrt(dx * dx + dy * dy) < 1.0f) {
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
                auto it_acc = ctx.uidToAccount.find(target_uid);
                if (it_acc != ctx.uidToAccount.end()) {
                    auto it_player = ctx.playerMap.find(it_acc->second);
                    if (it_player != ctx.playerMap.end()) {
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
            else if (mon->GetState() == MonsterState::ATTACK) {
                uint64_t target_uid = mon->GetTargetUserId();
                bool target_found = false;
                auto it_acc = ctx.uidToAccount.find(target_uid);
                if (it_acc != ctx.uidToAccount.end()) {
                    auto it_player = ctx.playerMap.find(it_acc->second);
                    if (it_player != ctx.playerMap.end()) {
                        float dx = it_player->second.x - old_x;
                        float dy = it_player->second.y - old_y;
                        if (std::sqrt(dx * dx + dy * dy) <= 3.0f) {
                            mon->UpdateTargetPosition({ it_player->second.x, it_player->second.y, 0.0f });
                            target_found = true;
                        }
                    }
                }
                if (!target_found) mon->GiveUpAttack();
            }

            mon->Update(delta_time);

            float new_x = mon->GetPosition().x;
            float new_y = mon->GetPosition().y;

            if (std::abs(old_x - new_x) > 0.05f || std::abs(old_y - new_y) > 0.05f) {
                ctx.zone->UpdatePosition(mon->GetId(), old_x, old_y, new_x, new_y);
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

                        for (uint64_t target_uid : aoi_uids) {
                            auto it = ctx.uidToAccount.find(target_uid);
                            if (it != ctx.uidToAccount.end()) {
                                s2s_res.add_target_account_ids(it->second);
                            }
                        }
                        ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
                    }
                }
            }
        }

        ScheduleNextAITick();
        }));
}

void StartAITickThread() {
    auto& ctx = GameContext::Get(); // ★ 컨텍스트 소환
    g_ai_timer = std::make_unique<boost::asio::steady_timer>(ctx.io_context);
    g_last_ai_time = std::chrono::steady_clock::now();

    ScheduleNextAITick();
    std::cout << "[MonsterManager] 비동기 Strand 기반 AI 타이머 루프 가동 시작 (10 FPS)\n";
}