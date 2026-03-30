#include "GatewayHandlers.h"
#include "../../GameServer.h"
#include "../../Session/GatewaySession.h"
#include "../../Monster/Monster.h"
#include "../../../Common/Define/Define_Server.h"
#include "../../../Common/Define/GameConstants.h"
#include "../../../Common/Utils/Logger.h"
#include "../../../Common/Redis/RedisManager.h"

#include <iostream>
#include <boost/asio/post.hpp>
#include <cmath>
#include <shared_mutex>

// ==========================================
// [락 설계 개선] playerMutex_ / monsterMutex_ 분리 적용
//
// 변경 전: 단일 gameStateMutex로 플레이어+몬스터 동시 보호
//   -> 몬스터 AI Tick 중 플레이어 이동이 대기
//   -> 대규모 동접 시 심각한 병목
//
// 변경 후: 각 핸들러에서 필요한 락만 최소 범위로 획득
//   -> playerMutex_: playerMap, uidToAccount 보호
//   -> monsterMutex_: monsterMap 보호 (공격 시에만 사용)
//   -> uidCounter: atomic 연산으로 락 불필요
//
// [수정] 모든 락 타입을 UTILITY::WriteLock/ReadLock으로 통일
// ==========================================

// [게이트웨이 -> 게임서버] 유저 이동 처리
void Handle_GatewayGameMoveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {

    auto req = std::make_shared<Protocol::GatewayGameMoveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("GameServer", "ParseFromArray 실패: " << __func__ << " (payloadSize=" << payloadSize << ")");
        return;
    }

    auto& ctx = GameContext::Get();

    std::string acc_id = req->account_id();
    float new_x = req->x();
    float new_y = req->y();

    // 맵 이탈 방지
    if (new_x < 0.0f) new_x = 0.0f;
    if (new_y < 0.0f) new_y = 0.0f;
    if (new_x > GameConstants::Map::WIDTH) new_x = GameConstants::Map::WIDTH;
    if (new_y > GameConstants::Map::HEIGHT) new_y = GameConstants::Map::HEIGHT;

    std::shared_ptr<PlayerInfo> player_ptr;

    // 1단계: playerMutex_ 읽기 락으로 유저 검색
    {
        UTILITY::ReadLock read_lock(ctx.playerMutex_);
        auto it = ctx.playerMap.find(acc_id);
        if (it != ctx.playerMap.end()) player_ptr = it->second;
    }

    // 2단계: 유저가 없다면 신규 진입 (Double-checked locking)
    if (!player_ptr) {
        bool is_new = false;

        {
            UTILITY::WriteLock write_lock(ctx.playerMutex_);
            auto it = ctx.playerMap.find(acc_id);
            if (it == ctx.playerMap.end()) {
                // atomic uidCounter: 락 없이 안전한 UID 발급
                uint64_t new_uid = ctx.uidCounter.fetch_add(1, std::memory_order_relaxed);
                player_ptr = std::make_shared<PlayerInfo>();
                player_ptr->uid = new_uid;
                player_ptr->x = new_x;
                player_ptr->y = new_y;

                ctx.playerMap[acc_id] = player_ptr;
                ctx.uidToAccount[new_uid] = acc_id;
                is_new = true;
            }
            else {
                player_ptr = it->second;
            }
        }

        if (is_new) {
#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
            if (acc_id.find("BOT_STRESS") != std::string::npos) {
                ctx.connected_bot_count.fetch_add(1, std::memory_order_relaxed);
            }
#endif
            ctx.zone->EnterZone(player_ptr->uid, new_x, new_y);
            LOG_INFO("GameServer", "유저(" << acc_id << ") 최초 Zone 진입 (UID:" << player_ptr->uid << ")");
            RedisManager::GetInstance().SetPlayerOnline(acc_id, GameConstants::Player::DEFAULT_HP);
        }
    }

    // 3단계: 좌표 갱신 + Zone::UpdatePosition (개별 유저 락)
    {
        std::lock_guard<std::mutex> p_lock(player_ptr->mtx);
        float old_x = player_ptr->x;
        float old_y = player_ptr->y;

        player_ptr->x = new_x;
        player_ptr->y = new_y;

        ctx.zone->UpdatePosition(player_ptr->uid, old_x, old_y, new_x, new_y);
    }

    auto aoi_uids = ctx.zone->GetPlayersInAOI(new_x, new_y);
    Protocol::GameGatewayMoveRes s2s_res;
    s2s_res.set_account_id(acc_id);
    s2s_res.set_x(new_x);
    s2s_res.set_y(new_y);

    // AOI 대상 account_id 조회: playerMutex_ 읽기 락
    {
        UTILITY::ReadLock read_lock(ctx.playerMutex_);

        int broadcast_limit = 0;
        for (uint64_t target_uid : aoi_uids) {
            if (broadcast_limit++ >= GameConstants::Network::MAX_AOI_BROADCAST) break;
            auto it = ctx.uidToAccount.find(target_uid);
            if (it != ctx.uidToAccount.end()) {
                s2s_res.add_target_account_ids(it->second);
            }
        }
    }
    
    session->Send(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
}

// [게이트웨이 -> 게임서버] 유저 퇴장 처리 핸들러
void Handle_GatewayGameLeaveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::GatewayGameLeaveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("GameServer", "ParseFromArray 실패: " << __func__ << " (payloadSize=" << payloadSize << ")");
        return;
    }

    auto& ctx = GameContext::Get();
    std::string acc_id = req->account_id();

    uint64_t uid = 0;
    float last_x = 0.0f;
    float last_y = 0.0f;
    bool found = false;

    // playerMutex_ 쓰기 락으로 삭제
    {
        UTILITY::WriteLock write_lock(ctx.playerMutex_);
        auto it = ctx.playerMap.find(acc_id);
        if (it != ctx.playerMap.end()) {
            uid = it->second->uid;
            last_x = it->second->x;
            last_y = it->second->y;

            ctx.uidToAccount.erase(uid);
            ctx.playerMap.erase(it);
            found = true;
        }
    }

    if (found) {
#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
        if (acc_id.find("BOT_STRESS") != std::string::npos) {
            ctx.connected_bot_count.fetch_sub(1, std::memory_order_relaxed);
        }
#endif
        // Zone 조작은 playerMutex_ 밖에서 수행 (데드락 방지)
        ctx.zone->LeaveZone(uid, last_x, last_y);
        LOG_INFO("GameServer", "유저(" << acc_id << ", UID:" << uid << ") 퇴장 완료. Zone에서 삭제됨.");
        RedisManager::GetInstance().RemovePlayer(acc_id);
    }
}

// [게이트웨이 -> 게임서버] 유저의 공격 요청 처리
void Handle_GatewayGameAttackReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t size) {

    auto req = std::make_shared<Protocol::GatewayGameAttackReq>();
    if (!req->ParseFromArray(payload, size)) {
        LOG_ERROR("GameServer", "ParseFromArray 실패: " << __func__ << " (payloadSize=" << size << ")");
        return;
    }

    auto& ctx = GameContext::Get();
    std::string account_id = req->account_id();
    std::shared_ptr<PlayerInfo> player_ptr;

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
    if (account_id.find("BOT_STRESS") != std::string::npos) {
        ctx.processed_packet_count.fetch_add(1, std::memory_order_relaxed);
    }
#endif

    // playerMutex_ 읽기 락: 플레이어 검색
    {
        UTILITY::ReadLock read_lock(ctx.playerMutex_);
        auto it_player = ctx.playerMap.find(account_id);
        if (it_player == ctx.playerMap.end()) return;
        player_ptr = it_player->second;
    }

    float p_x, p_y;
    int p_atk;
    uint64_t p_uid;
    {
        std::lock_guard<std::mutex> p_lock(player_ptr->mtx);
        p_x = player_ptr->x;
        p_y = player_ptr->y;
        p_atk = player_ptr->atk;
        p_uid = player_ptr->uid;
    }

    std::shared_ptr<Monster> target_monster = nullptr;
    float min_dist = GameConstants::Combat::PLAYER_ATTACK_RANGE;

    auto aoi_mon_ids = ctx.zone->GetMonstersInAOI(p_x, p_y);

    // monsterMutex_ 읽기 락: 몬스터 검색 (playerMutex_와 독립)
    {
        UTILITY::ReadLock read_lock(ctx.monsterMutex_);
        for (uint64_t mon_id : aoi_mon_ids) {
            auto it_mon = ctx.monsterMap.find(mon_id);
            if (it_mon == ctx.monsterMap.end()) continue;

            auto& mon = it_mon->second;
            if (mon->GetState() == MonsterState::DEAD) continue;

            float dx = p_x - mon->GetPosition().x;
            float dy = p_y - mon->GetPosition().y;
            float dist = std::sqrt(dx * dx + dy * dy);

            if (dist <= min_dist) {
                target_monster = mon;
                min_dist = dist;
            }
        }
    }

    // 사거리 내에 몬스터가 없을 경우
    if (!target_monster) {
        Protocol::GameGatewayAttackRes fail_res;
        fail_res.set_attacker_uid(p_uid);
        fail_res.set_damage(0);
        fail_res.add_target_account_ids(account_id);
        ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, fail_res);
        return;
    }

    // 데미지 연산 (모든 글로벌 락 밖에서 안전하게 수행)
    int damage = p_atk - target_monster->GetDef();
    if (damage < GameConstants::Combat::MIN_DAMAGE) damage = GameConstants::Combat::MIN_DAMAGE;

    int remain_hp = target_monster->TakeDamage(damage);

    LOG_INFO("Combat", "유저(" << account_id << ")가 몬스터(ID:"
        << target_monster->GetId() << ") 공격! 데미지: " << damage);

    Protocol::GameGatewayAttackRes s2s_res;
    s2s_res.set_attacker_uid(p_uid);
    s2s_res.set_target_uid(target_monster->GetId());
    s2s_res.set_target_account_id("MONSTER_" + std::to_string(target_monster->GetId()));
    s2s_res.set_damage(damage);
    s2s_res.set_target_remain_hp(remain_hp);

    auto aoi_uids = ctx.zone->GetPlayersInAOI(p_x, p_y);
    // playerMutex_ 읽기 락: AOI 유저 이름 조회
    {
        UTILITY::ReadLock read_lock(ctx.playerMutex_);

        int broadcast_limit = 0;
        for (uint64_t uid : aoi_uids) {
            if (broadcast_limit++ >= GameConstants::Network::MAX_AOI_BROADCAST) break;
            auto target_acc = ctx.uidToAccount.find(uid);
            if (target_acc != ctx.uidToAccount.end()) {
                s2s_res.add_target_account_ids(target_acc->second);
            }
        }
    }

    ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, s2s_res);

    if (remain_hp <= 0) {
        LOG_INFO("System", "몬스터(ID:" << target_monster->GetId() << ")가 쓰러졌습니다!");
        target_monster->Die();
    }
}

// ==========================================
// [추가] 채팅 AOI 처리 핸들러
//
// 변경 전: GatewayServer에서 clientMap 전체를 순회하여 모든 유저에게 브로드캐스트
//   -> 동접 수천~수만 명일 때 즉시 병목
//
// 변경 후: GameServer의 Zone/AOI 시스템을 활용하여 주변 유저에게만 전달
//   -> GatewayServer에서 S2S로 채팅 요청 수신
//   -> 발신자의 위치 기반 AOI 내 유저 목록을 계산
//   -> 대상 account_id 리스트와 함께 응답
// ==========================================
void Handle_GatewayGameChatReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    Protocol::GatewayGameChatReq req;
    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("GameServer", "ParseFromArray 실패: " << __func__ << " (payloadSize=" << payloadSize << ")");
        return;
    }

    auto& ctx = GameContext::Get();
    std::string acc_id = req.account_id();

    // 발신자 위치 조회
    float p_x = 0.0f, p_y = 0.0f;
    {
        UTILITY::ReadLock read_lock(ctx.playerMutex_);
        auto it = ctx.playerMap.find(acc_id);
        if (it == ctx.playerMap.end()) {
            LOG_WARN("GameServer", "채팅 발신자가 playerMap에 없음: " << acc_id);
            return;
        }
        std::lock_guard<std::mutex> p_lock(it->second->mtx);
        p_x = it->second->x;
        p_y = it->second->y;
    }

    // AOI 내 유저 목록 조회
    auto aoi_uids = ctx.zone->GetPlayersInAOI(p_x, p_y);

    Protocol::GameGatewayChatRes s2s_res;
    s2s_res.set_account_id(acc_id);
    s2s_res.set_msg(req.msg());

    {
        UTILITY::ReadLock read_lock(ctx.playerMutex_);
        for (uint64_t uid : aoi_uids) {
            auto it = ctx.uidToAccount.find(uid);
            if (it != ctx.uidToAccount.end()) {
                s2s_res.add_target_account_ids(it->second);
            }
        }
    }

    session->Send(Protocol::PKT_GAME_GATEWAY_CHAT_RES, s2s_res);
}
