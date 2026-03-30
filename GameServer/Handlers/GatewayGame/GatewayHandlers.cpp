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

// ==========================================
// [스레드 모델 재설계] game_strand_ 기반 단일 스레드 게임 로직
//
// 변경 전: shared_mutex(playerMutex_, monsterMutex_)와
//   PlayerInfo::mtx(개별 뮤텍스)를 조합하여 멀티스레드 보호
//   -> 락 순서 규칙을 수동 관리, 데드락 위험 잠재
//   -> 몬스터 AI Tick과 이동 핸들러 간 락 경합
//
// 변경 후: 모든 핸들러가 game_strand_에서 실행되므로
//   playerMap, monsterMap, PlayerInfo에 대한 동시 접근이 불가능
//   -> 모든 ReadLock/WriteLock 제거
//   -> 모든 lock_guard<mutex>(player_ptr->mtx) 제거
//   -> 락 순서 규칙 자체가 불필요 (데드락 원천 제거)
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

    // [수정] game_strand_ 보호 하에 직접 접근 (뮤텍스 불필요)
    auto it = ctx.playerMap.find(acc_id);
    std::shared_ptr<PlayerInfo> player_ptr;

    if (it != ctx.playerMap.end()) {
        player_ptr = it->second;
    }
    else {
        // 신규 유저 진입
        uint64_t new_uid = ctx.uidCounter.fetch_add(1, std::memory_order_relaxed);
        player_ptr = std::make_shared<PlayerInfo>();
        player_ptr->uid = new_uid;
        player_ptr->x = new_x;
        player_ptr->y = new_y;

        ctx.playerMap[acc_id] = player_ptr;
        ctx.uidToAccount[new_uid] = acc_id;

        // [추가] 게이트웨이 소속 유저 등록 (장애 복구용)
        ctx.RegisterPlayerToGateway(session.get(), acc_id);

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
        if (acc_id.find("BOT_STRESS") != std::string::npos) {
            ctx.connected_bot_count.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        ctx.zone->EnterZone(player_ptr->uid, new_x, new_y);
        LOG_INFO("GameServer", "유저(" << acc_id << ") 최초 Zone 진입 (UID:" << player_ptr->uid << ")");
        RedisManager::GetInstance().SetPlayerOnline(acc_id, GameConstants::Player::DEFAULT_HP);
    }

    // 좌표 갱신 + Zone 위치 업데이트 (game_strand_ 보호, 뮤텍스 불필요)
    float old_x = player_ptr->x;
    float old_y = player_ptr->y;
    player_ptr->x = new_x;
    player_ptr->y = new_y;
    ctx.zone->UpdatePosition(player_ptr->uid, old_x, old_y, new_x, new_y);

    auto aoi_uids = ctx.zone->GetPlayersInAOI(new_x, new_y);
    Protocol::GameGatewayMoveRes s2s_res;
    s2s_res.set_account_id(acc_id);
    s2s_res.set_x(new_x);
    s2s_res.set_y(new_y);

    // AOI 대상 account_id 조회 (game_strand_ 보호, 뮤텍스 불필요)
    int broadcast_limit = 0;
    for (uint64_t target_uid : aoi_uids) {
        if (broadcast_limit++ >= GameConstants::Network::MAX_AOI_BROADCAST) break;
        auto uid_it = ctx.uidToAccount.find(target_uid);
        if (uid_it != ctx.uidToAccount.end()) {
            s2s_res.add_target_account_ids(uid_it->second);
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

    // [수정] game_strand_ 보호 하에 직접 접근 (뮤텍스 불필요)
    auto it = ctx.playerMap.find(acc_id);
    if (it != ctx.playerMap.end()) {
        uint64_t uid = it->second->uid;
        float last_x = it->second->x;
        float last_y = it->second->y;

        ctx.uidToAccount.erase(uid);
        ctx.playerMap.erase(it);

        // [추가] 게이트웨이 소속에서 제거
        ctx.UnregisterPlayerFromGateway(session.get(), acc_id);

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
        if (acc_id.find("BOT_STRESS") != std::string::npos) {
            ctx.connected_bot_count.fetch_sub(1, std::memory_order_relaxed);
        }
#endif
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

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
    if (account_id.find("BOT_STRESS") != std::string::npos) {
        ctx.processed_packet_count.fetch_add(1, std::memory_order_relaxed);
    }
#endif

    // [수정] game_strand_ 보호 하에 직접 접근 (뮤텍스 불필요)
    auto it_player = ctx.playerMap.find(account_id);
    if (it_player == ctx.playerMap.end()) return;
    auto& player_ptr = it_player->second;

    float p_x = player_ptr->x;
    float p_y = player_ptr->y;
    int p_atk = player_ptr->atk;
    uint64_t p_uid = player_ptr->uid;

    std::shared_ptr<Monster> target_monster = nullptr;
    float min_dist = GameConstants::Combat::PLAYER_ATTACK_RANGE;

    auto aoi_mon_ids = ctx.zone->GetMonstersInAOI(p_x, p_y);

    // game_strand_ 보호 하에 직접 접근 (뮤텍스 불필요)
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

    // 사거리 내에 몬스터가 없을 경우
    if (!target_monster) {
        Protocol::GameGatewayAttackRes fail_res;
        fail_res.set_attacker_uid(p_uid);
        fail_res.set_damage(0);
        fail_res.add_target_account_ids(account_id);
        ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, fail_res);
        return;
    }

    // 데미지 연산
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
    int broadcast_limit = 0;
    for (uint64_t uid : aoi_uids) {
        if (broadcast_limit++ >= GameConstants::Network::MAX_AOI_BROADCAST) break;
        auto target_acc = ctx.uidToAccount.find(uid);
        if (target_acc != ctx.uidToAccount.end()) {
            s2s_res.add_target_account_ids(target_acc->second);
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
// ==========================================
void Handle_GatewayGameChatReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    Protocol::GatewayGameChatReq req;
    if (!req.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("GameServer", "ParseFromArray 실패: " << __func__ << " (payloadSize=" << payloadSize << ")");
        return;
    }

    auto& ctx = GameContext::Get();
    std::string acc_id = req.account_id();

    // [수정] game_strand_ 보호 하에 직접 접근 (뮤텍스 불필요)
    auto it = ctx.playerMap.find(acc_id);
    if (it == ctx.playerMap.end()) {
        LOG_WARN("GameServer", "채팅 발신자가 playerMap에 없음: " << acc_id);
        return;
    }
    float p_x = it->second->x;
    float p_y = it->second->y;

    auto aoi_uids = ctx.zone->GetPlayersInAOI(p_x, p_y);

    Protocol::GameGatewayChatRes s2s_res;
    s2s_res.set_account_id(acc_id);
    s2s_res.set_msg(req.msg());

    for (uint64_t uid : aoi_uids) {
        auto uid_it = ctx.uidToAccount.find(uid);
        if (uid_it != ctx.uidToAccount.end()) {
            s2s_res.add_target_account_ids(uid_it->second);
        }
    }

    session->Send(Protocol::PKT_GAME_GATEWAY_CHAT_RES, s2s_res);
}
