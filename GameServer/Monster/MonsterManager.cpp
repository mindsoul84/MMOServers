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
#include "../../Common/Define/GameConstants.h"
#include "../../Common/Utils/Logger.h"

#include <iostream>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <boost/asio.hpp>

static std::unique_ptr<boost::asio::steady_timer> g_ai_timer;
static auto g_last_ai_time = std::chrono::steady_clock::now();
static std::unordered_map<uint64_t, float> g_sync_timers;

// ==========================================
// 몬스터 초기 스폰 함수
//
//   game_strand_ 도입에 따라 공격 콜백 내부의
// playerMutex_ ReadLock, PlayerInfo::mtx lock_guard 전부 제거.
// 콜백은 AI Tick에서 호출되며, AI Tick은 game_strand_에서 실행되므로
// playerMap, PlayerInfo에 대한 동시 접근이 원천적으로 불가능합니다.
// ==========================================
void InitMonsters() {
    auto& ctx = GameContext::Get();

    const auto& spawnList = ctx.dataManager.GetMonsterData().GetMonsterSpawnList();

    for (const auto& spawn_data : spawnList) {
        uint64_t mon_id = spawn_data.mon_id;
        auto mon = std::make_shared<Monster>(mon_id, &ctx.navMesh);

        mon->SetOnAttackCallback([](uint64_t attacker_uid, uint64_t target_uid, int damage) {
            auto& ctx_inner = GameContext::Get();

            //   game_strand_ 보호 하에 직접 접근 (뮤텍스 불필요)
            auto it_acc = ctx_inner.uidToAccount.find(target_uid);
            if (it_acc == ctx_inner.uidToAccount.end()) return;
            std::string acc_id_str = it_acc->second;

            auto it_player = ctx_inner.playerMap.find(acc_id_str);
            if (it_player == ctx_inner.playerMap.end()) return;
            auto& player_ptr = it_player->second;

            //   PlayerInfo::mtx 제거 — game_strand_ 보호
            player_ptr->hp -= damage;
            if (player_ptr->hp < 0) player_ptr->hp = 0;
            int remain_hp = player_ptr->hp;
            float p_x = player_ptr->x;
            float p_y = player_ptr->y;

            RedisManager::GetInstance().UpdatePlayerHp(acc_id_str, remain_hp);

            Protocol::GameGatewayAttackRes s2s_res;
            s2s_res.set_attacker_uid(attacker_uid);
            s2s_res.set_target_uid(target_uid);
            s2s_res.set_target_account_id(acc_id_str);
            s2s_res.set_damage(damage);
            s2s_res.set_target_remain_hp(remain_hp);

            auto aoi_uids = ctx_inner.zone->GetPlayersInAOI(p_x, p_y);

            for (uint64_t aoi_uid : aoi_uids) {
                auto target_acc = ctx_inner.uidToAccount.find(aoi_uid);
                if (target_acc != ctx_inner.uidToAccount.end()) {
                    s2s_res.add_target_account_ids(target_acc->second);
                }
            }

            ctx_inner.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, s2s_res);

            LOG_INFO("Combat", "몬스터(" << attacker_uid << ")가 유저(" << acc_id_str
                << ")를 공격! 데미지: " << damage << ", 남은 체력: " << remain_hp);

            // 유저 기절 시 마을로 텔레포트 처리
            if (remain_hp <= 0) {
                LOG_INFO("System", "유저(" << acc_id_str << ") 체력 0. 마을(0,0)로 복귀.");

                float old_x = p_x;
                float old_y = p_y;

                //   PlayerInfo::mtx 제거 — game_strand_ 보호
                player_ptr->hp = GameConstants::Player::DEFAULT_HP;
                player_ptr->x = GameConstants::Player::SPAWN_X;
                player_ptr->y = GameConstants::Player::SPAWN_Y;

                // Redis HP 갱신 (부활: HP 100으로 복구)
                RedisManager::GetInstance().UpdatePlayerHp(acc_id_str, GameConstants::Player::DEFAULT_HP);
                ctx_inner.zone->UpdatePosition(target_uid, old_x, old_y,
                    GameConstants::Player::SPAWN_X, GameConstants::Player::SPAWN_Y);

                Protocol::GameGatewayMoveRes teleport_res;
                teleport_res.set_account_id(acc_id_str);
                teleport_res.set_x(GameConstants::Player::SPAWN_X);
                teleport_res.set_y(GameConstants::Player::SPAWN_Y);
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

        LOG_INFO("MonsterManager", "[스폰] 몬스터(ID:" << mon_id << ", HP:" << spawn_data.hp
            << ", 리스폰:" << spawn_data.respawn_sec << "초) 좌표 (X:"
            << mon->GetPosition().x << ", Y:" << mon->GetPosition().y << ")");
    }
    LOG_INFO("MonsterManager", "몬스터 " << ctx.monsters.size() << "마리 스폰 완료 및 Zone 등록됨.");
}

// ==========================================
// [리팩토링] 상태별 처리 함수 분리
//
//   game_strand_에서 실행되므로 모든 뮤텍스 제거.
// 기존 락 순서 규칙(monsterMutex_ -> playerMutex_)이 필요 없어짐.
// 아래 함수들은 모두 ScheduleNextAITick의 game_strand_ 콜백 내에서 호출됩니다.
// ==========================================

// [분리] 몬스터 사망 상태 처리
void ProcessDeadMonster(std::shared_ptr<Monster>& mon, float delta_time) {
    auto& ctx = GameContext::Get();

    mon->AddDeadTime(delta_time);

    if (mon->GetDeadTime() < mon->GetRespawnSec()) return;

    mon->Respawn();
    LOG_INFO("System", "몬스터(ID:" << mon->GetId() << ")가 " << mon->GetRespawnSec() << "초 후 리스폰.");

    // 부활 사실을 주변 유저에게 알림
    auto aoi_uids = ctx.zone->GetPlayersInAOI(mon->GetPosition().x, mon->GetPosition().y);
    if (aoi_uids.empty()) return;

    Protocol::GameGatewayMoveRes s2s_res;
    s2s_res.set_account_id("MONSTER_" + std::to_string(mon->GetId()));
    s2s_res.set_x(mon->GetPosition().x);
    s2s_res.set_y(mon->GetPosition().y);
    s2s_res.set_z(mon->GetPosition().z);
    s2s_res.set_yaw(0.0f);

    //   game_strand_ 보호 — 뮤텍스 불필요
    for (uint64_t target_uid : aoi_uids) {
        auto it = ctx.uidToAccount.find(target_uid);
        if (it != ctx.uidToAccount.end()) {
            s2s_res.add_target_account_ids(it->second);
        }
    }
    ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
}

// [분리] IDLE 상태: 주변 유저 탐색하여 어그로 발동
void ProcessIdleMonster(std::shared_ptr<Monster>& mon, float old_x, float old_y) {
    auto& ctx = GameContext::Get();

    auto aoi_uids = ctx.zone->GetPlayersInAOI(old_x, old_y);

    //   game_strand_ 보호 — 뮤텍스 불필요
    for (uint64_t uid : aoi_uids) {
        auto it_acc = ctx.uidToAccount.find(uid);
        if (it_acc == ctx.uidToAccount.end()) continue;

        auto it_player = ctx.playerMap.find(it_acc->second);
        if (it_player == ctx.playerMap.end()) continue;

        float dx = it_player->second->x - old_x;
        float dy = it_player->second->y - old_y;
        if (std::sqrt(dx * dx + dy * dy) < GameConstants::Monster::AGGRO_RANGE) {
            mon->SetTarget(uid, { it_player->second->x, it_player->second->y, 0.0f });
            break;
        }
    }
}

// [분리] CHASE 상태: 타겟 추적 유지 또는 포기
void ProcessChaseMonster(std::shared_ptr<Monster>& mon, float old_x, float old_y) {
    auto& ctx = GameContext::Get();
    uint64_t target_uid = mon->GetTargetUserId();
    bool target_found = false;

    //   game_strand_ 보호 — 뮤텍스 불필요
    auto it_acc = ctx.uidToAccount.find(target_uid);
    if (it_acc != ctx.uidToAccount.end()) {
        auto it_player = ctx.playerMap.find(it_acc->second);
        if (it_player != ctx.playerMap.end()) {
            float dx = it_player->second->x - old_x;
            float dy = it_player->second->y - old_y;
            if (std::sqrt(dx * dx + dy * dy) <= GameConstants::Monster::CHASE_RANGE) {
                mon->UpdateTargetPosition({ it_player->second->x, it_player->second->y, 0.0f });
                target_found = true;
            }
        }
    }

    if (!target_found) mon->GiveUpChase();
}

// [분리] ATTACK 상태: 타겟 유지 또는 포기
void ProcessAttackMonster(std::shared_ptr<Monster>& mon, float old_x, float old_y) {
    auto& ctx = GameContext::Get();
    uint64_t target_uid = mon->GetTargetUserId();
    bool target_found = false;

    //   game_strand_ 보호 — 뮤텍스 불필요
    auto it_acc = ctx.uidToAccount.find(target_uid);
    if (it_acc != ctx.uidToAccount.end()) {
        auto it_player = ctx.playerMap.find(it_acc->second);
        if (it_player != ctx.playerMap.end()) {
            float dx = it_player->second->x - old_x;
            float dy = it_player->second->y - old_y;
            if (std::sqrt(dx * dx + dy * dy) <= GameConstants::Monster::CHASE_RANGE) {
                mon->UpdateTargetPosition({ it_player->second->x, it_player->second->y, 0.0f });
                target_found = true;
            }
        }
    }

    if (!target_found) mon->GiveUpAttack();
}

// [분리] 몬스터 이동 후 Zone 갱신 및 네트워크 동기화
void SyncMonsterPosition(std::shared_ptr<Monster>& mon, float old_x, float old_y, float delta_time) {
    auto& ctx = GameContext::Get();

    float new_x = mon->GetPosition().x;
    float new_y = mon->GetPosition().y;

    if (std::abs(old_x - new_x) <= GameConstants::AI::POSITION_EPSILON &&
        std::abs(old_y - new_y) <= GameConstants::AI::POSITION_EPSILON) {
        return;
    }

    ctx.zone->UpdatePositionMonster(mon->GetId(), old_x, old_y, new_x, new_y);
    g_sync_timers[mon->GetId()] += delta_time;

    if (g_sync_timers[mon->GetId()] < GameConstants::Network::MONSTER_SYNC_INTERVAL) return;

    g_sync_timers[mon->GetId()] = 0.0f;
    auto aoi_uids = ctx.zone->GetPlayersInAOI(new_x, new_y);

    if (aoi_uids.empty()) return;

    Protocol::GameGatewayMoveRes s2s_res;
    s2s_res.set_account_id("MONSTER_" + std::to_string(mon->GetId()));
    s2s_res.set_x(new_x);
    s2s_res.set_y(new_y);
    s2s_res.set_z(mon->GetPosition().z);
    s2s_res.set_yaw(0.0f);

    //   game_strand_ 보호 — 뮤텍스 불필요
    for (uint64_t target_uid : aoi_uids) {
        auto it = ctx.uidToAccount.find(target_uid);
        if (it != ctx.uidToAccount.end()) {
            s2s_res.add_target_account_ids(it->second);
        }
    }
    ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
}

// ==========================================
//   ScheduleNextAITick - game_strand_ 바인딩
//
// 변경 전: 타이머 콜백이 io_context의 아무 워커 스레드에서 실행됨
//   -> monsterMutex_ shared_lock 필요
//   -> 내부에서 playerMutex_도 추가 획득하여 락 중첩 발생
//
// 변경 후: bind_executor(game_strand_)로 콜백을 바인딩
//   -> 패킷 핸들러와 동일한 strand에서 실행됨
//   -> playerMap, monsterMap 접근 시 뮤텍스 불필요
//   -> Monster::Update()도 strand에서 실행되므로 스레드 안전
// ==========================================
void ScheduleNextAITick() {
    g_ai_timer->expires_after(std::chrono::milliseconds(GameConstants::AI::TICK_INTERVAL_MS));

    auto& ctx = GameContext::Get();
    g_ai_timer->async_wait(
        boost::asio::bind_executor(ctx.game_strand_, [](const boost::system::error_code& ec) {
            if (ec) return;

            auto& ctx = GameContext::Get();

            auto current_time = std::chrono::steady_clock::now();
            float delta_time = std::chrono::duration<float>(current_time - g_last_ai_time).count();
            g_last_ai_time = current_time;

            //   game_strand_에서 실행 — monsterMutex_ 불필요
            for (auto& mon : ctx.monsters) {

                // 1. 사망 상태 처리
                if (mon->GetState() == MonsterState::DEAD) {
                    ProcessDeadMonster(mon, delta_time);
                    continue;
                }

                float old_x = mon->GetPosition().x;
                float old_y = mon->GetPosition().y;

                // 2. 상태별 처리
                switch (mon->GetState()) {
                case MonsterState::IDLE:
                    ProcessIdleMonster(mon, old_x, old_y);
                    break;
                case MonsterState::CHASE:
                    ProcessChaseMonster(mon, old_x, old_y);
                    break;
                case MonsterState::ATTACK:
                    ProcessAttackMonster(mon, old_x, old_y);
                    break;
                default:
                    break;
                }

                // 3. AI 업데이트 (이동) — game_strand_에서 실행되므로 스레드 안전
                mon->Update(delta_time);

                // 4. 위치 동기화
                SyncMonsterPosition(mon, old_x, old_y, delta_time);
            }

            ScheduleNextAITick();
        })
    );
}

void StartAITickThread() {
    auto& ctx = GameContext::Get();
    g_ai_timer = std::make_unique<boost::asio::steady_timer>(ctx.io_context);
    g_last_ai_time = std::chrono::steady_clock::now();

    ScheduleNextAITick();
    LOG_INFO("MonsterManager", "비동기 game_strand_ 기반 AI 타이머 루프 가동 시작 (10 FPS)");
}
