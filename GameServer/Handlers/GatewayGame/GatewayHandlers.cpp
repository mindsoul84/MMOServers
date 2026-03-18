#include "GatewayHandlers.h"
#include "../../GameServer.h"
#include "../../Session/GatewaySession.h"
#include "../../Monster/Monster.h" // 몬스터 공격 처리를 위해 추가

#include <iostream>
#include <boost/asio/post.hpp>
#include <cmath> // std::sqrt
#include <shared_mutex>

// [게이트웨이 -> 게임서버] 유저 이동 처리
void Handle_GatewayGameMoveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::GatewayGameMoveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) return;

    auto& ctx = GameContext::Get();

    // ===================================================
    // 상태 변경(쓰기)이 일어나므로 unique_lock으로 보호
    // ===================================================
    std::unique_lock<std::shared_mutex> lock(ctx.gameStateMutex);

    std::string acc_id = req->account_id();
    float new_x = req->x();
    float new_y = req->y();

    // =========================================================
    // ★ [버그 픽스] 맵 이탈(음수 좌표 및 최대 크기 초과) 방지 로직 
    // 맵 크기(1000x1000) 밖으로 나가려고 하면 강제로 벽에 붙여버립니다.
    // =========================================================
    if (new_x < 0.0f) new_x = 0.0f;
    if (new_y < 0.0f) new_y = 0.0f;
    if (new_x > 1000.0f) new_x = 1000.0f; // Zone 생성 시 설정한 최대 Width
    if (new_y > 1000.0f) new_y = 1000.0f; // Zone 생성 시 설정한 최대 Height
    // =========================================================

    if (ctx.playerMap.find(acc_id) == ctx.playerMap.end()) {
        uint64_t new_uid = ctx.uidCounter++;
        ctx.playerMap[acc_id] = { new_uid, new_x, new_y };
        ctx.uidToAccount[new_uid] = acc_id;
        ctx.zone->EnterZone(new_uid, new_x, new_y);
        std::cout << "[GameServer] 유저(" << acc_id << ") 최초 Zone 진입 (UID:" << new_uid << ")\n";
    }
    else {
        auto& info = ctx.playerMap[acc_id];
        ctx.zone->UpdatePosition(info.uid, info.x, info.y, new_x, new_y);
        info.x = new_x;
        info.y = new_y;
    }

    auto aoi_uids = ctx.zone->GetPlayersInAOI(new_x, new_y);

    Protocol::GameGatewayMoveRes s2s_res;
    s2s_res.set_account_id(acc_id);
    s2s_res.set_x(new_x);
    s2s_res.set_y(new_y);

    for (uint64_t target_uid : aoi_uids) {
        auto it = ctx.uidToAccount.find(target_uid);
        if (it != ctx.uidToAccount.end()) {
            s2s_res.add_target_account_ids(it->second);
        }
    }
    session->Send(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
}


// [게이트웨이 -> 게임서버] 유저 퇴장 처리 핸들러
void Handle_GatewayGameLeaveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::GatewayGameLeaveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) return;

    auto& ctx = GameContext::Get();

    // 쓰기 락 적용
    std::unique_lock<std::shared_mutex> lock(ctx.gameStateMutex);

    std::string acc_id = req->account_id();
    auto it = ctx.playerMap.find(acc_id);
    if (it != ctx.playerMap.end()) {
        uint64_t uid = it->second.uid;
        float last_x = it->second.x;
        float last_y = it->second.y;

        ctx.zone->LeaveZone(uid, last_x, last_y);
        ctx.uidToAccount.erase(uid);
        ctx.playerMap.erase(it);

        std::cout << "[GameServer] 👻 유저(" << acc_id << ", UID:" << uid << ") 퇴장 완료. Zone에서 삭제됨.\n";
    }
}

// [게이트웨이 -> 게임서버] 유저의 공격 요청 처리
void Handle_GatewayGameAttackReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t size) {
    auto req = std::make_shared<Protocol::GatewayGameAttackReq>();
    if (!req->ParseFromArray(payload, size)) return;

    auto& ctx = GameContext::Get();
    
    std::unique_lock<std::shared_mutex> lock(ctx.gameStateMutex);   // 쓰기 락 적용

    // 공격 처리도 상태(Player, Monster)를 변경하므로 반드시 strand 안에서 실행해야 합니다!
    std::string account_id = req->account_id();

    auto it_player = ctx.playerMap.find(account_id);
    if (it_player == ctx.playerMap.end()) return;

    PlayerInfo& player = it_player->second;

    // 타겟 몬스터 탐색
    std::shared_ptr<Monster> target_monster = nullptr;
    float min_dist = 1.5f;

    // =========================================================
    // 전체 순회(O(N)) 제거! Zone 기반 O(1) 탐색!
    // =========================================================
    auto aoi_mon_ids = ctx.zone->GetMonstersInAOI(player.x, player.y);

    for (uint64_t mon_id : aoi_mon_ids) {
        auto it_mon = ctx.monsterMap.find(mon_id);
        if (it_mon == ctx.monsterMap.end()) continue;

        auto& mon = it_mon->second;
        if (mon->GetState() == MonsterState::DEAD) continue;

        float dx = player.x - mon->GetPosition().x;
        float dy = player.y - mon->GetPosition().y;
        float dist = std::sqrt(dx * dx + dy * dy);

        if (dist <= min_dist) {
            target_monster = mon;
            min_dist = dist;
        }
    }

    // 사거리 내에 몬스터가 없을 경우
    if (!target_monster) {
        Protocol::GameGatewayAttackRes fail_res;
        fail_res.set_attacker_uid(player.uid);
        fail_res.set_damage(0);
        fail_res.add_target_account_ids(account_id);
        ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, fail_res);
        return;
    }

    // 데미지 연산 및 적용
    int damage = player.atk - target_monster->GetDef();
    if (damage < 1) damage = 1;
    target_monster->TakeDamage(damage);

    std::cout << "[Combat] ⚔️ 유저(" << account_id << ")가 몬스터(ID:"
        << target_monster->GetId() << ") 공격! 데미지: " << damage << "\n";

    // 주변 유저에게 전투 결과 브로드캐스트
    Protocol::GameGatewayAttackRes s2s_res;
    s2s_res.set_attacker_uid(player.uid);
    s2s_res.set_target_uid(target_monster->GetId());
    s2s_res.set_target_account_id("MONSTER_" + std::to_string(target_monster->GetId()));
    s2s_res.set_damage(damage);
    s2s_res.set_target_remain_hp(target_monster->GetHp());

    auto aoi_uids = ctx.zone->GetPlayersInAOI(player.x, player.y);
    for (uint64_t uid : aoi_uids) {
        auto target_acc = ctx.uidToAccount.find(uid);
        if (target_acc != ctx.uidToAccount.end()) {
            s2s_res.add_target_account_ids(target_acc->second);
        }
    }

    ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, s2s_res);

    if (target_monster->GetHp() <= 0) {
        std::cout << "[System] 💀 몬스터(ID:" << target_monster->GetId() << ")가 쓰러졌습니다!\n";
        target_monster->Die();
    }
}