#include "GatewayHandlers.h"
#include "../../GameServer.h"
#include "../../Session/GatewaySession.h"
#include "../../Monster/Monster.h" // 몬스터 공격 처리를 위해 추가

#include <iostream>
#include <boost/asio/post.hpp>
#include <cmath> // std::sqrt

// [게이트웨이 -> 게임서버] 유저 이동 처리
void Handle_GatewayGameMoveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::GatewayGameMoveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) return;

    auto& ctx = GameContext::Get();

    // ★ g_game_strand 대신 ctx.game_strand 사용
    boost::asio::post(ctx.game_strand, [session, req]() {
        auto& ctx_inner = GameContext::Get(); // 람다 내부에서 다시 참조
        std::string acc_id = req->account_id();
        float new_x = req->x();
        float new_y = req->y();

        if (ctx_inner.playerMap.find(acc_id) == ctx_inner.playerMap.end()) {
            uint64_t new_uid = ctx_inner.uidCounter++;
            ctx_inner.playerMap[acc_id] = { new_uid, new_x, new_y };
            ctx_inner.uidToAccount[new_uid] = acc_id;
            ctx_inner.zone->EnterZone(new_uid, new_x, new_y);
            std::cout << "[GameServer] 유저(" << acc_id << ") 최초 Zone 진입 (UID:" << new_uid << ")\n";
        }
        else {
            auto& info = ctx_inner.playerMap[acc_id];
            ctx_inner.zone->UpdatePosition(info.uid, info.x, info.y, new_x, new_y);
            info.x = new_x;
            info.y = new_y;
        }

        auto aoi_uids = ctx_inner.zone->GetPlayersInAOI(new_x, new_y);

        Protocol::GameGatewayMoveRes s2s_res;
        s2s_res.set_account_id(acc_id);
        s2s_res.set_x(new_x);
        s2s_res.set_y(new_y);
        // ...

        for (uint64_t target_uid : aoi_uids) {
            if (target_uid < 10000) {
                auto it = ctx_inner.uidToAccount.find(target_uid);
                if (it != ctx_inner.uidToAccount.end()) {
                    s2s_res.add_target_account_ids(it->second);
                }
            }
        }
        session->Send(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
        });
}


// [게이트웨이 -> 게임서버] 유저 퇴장 처리 핸들러
void Handle_GatewayGameLeaveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::GatewayGameLeaveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) return;

    auto& ctx = GameContext::Get();

    boost::asio::post(ctx.game_strand, [req]() {
        auto& ctx_inner = GameContext::Get();
        std::string acc_id = req->account_id();

        auto it = ctx_inner.playerMap.find(acc_id);
        if (it != ctx_inner.playerMap.end()) {
            uint64_t uid = it->second.uid;
            float last_x = it->second.x;
            float last_y = it->second.y;

            ctx_inner.zone->LeaveZone(uid, last_x, last_y);
            ctx_inner.uidToAccount.erase(uid);
            ctx_inner.playerMap.erase(it);

            std::cout << "[GameServer] 👻 유저(" << acc_id << ", UID:" << uid << ") 퇴장 완료. Zone에서 삭제됨.\n";
        }
    });
}

// [게이트웨이 -> 게임서버] 유저의 공격 요청 처리
void Handle_GatewayGameAttackReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t size) {
    auto req = std::make_shared<Protocol::GatewayGameAttackReq>();
    if (!req->ParseFromArray(payload, size)) return;

    auto& ctx = GameContext::Get();

    // 공격 처리도 상태(Player, Monster)를 변경하므로 반드시 strand 안에서 실행해야 합니다!
    boost::asio::post(ctx.game_strand, [req]() {
        auto& ctx_inner = GameContext::Get();
        std::string account_id = req->account_id();

        auto it_player = ctx_inner.playerMap.find(account_id);
        if (it_player == ctx_inner.playerMap.end()) return;

        PlayerInfo& player = it_player->second;

        // 타겟 몬스터 탐색
        std::shared_ptr<Monster> target_monster = nullptr;
        float min_dist = 1.5f;

        for (auto& mon : ctx_inner.monsters) {
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
            ctx_inner.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, fail_res);
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

        auto aoi_uids = ctx_inner.zone->GetPlayersInAOI(player.x, player.y);
        for (uint64_t uid : aoi_uids) {
            auto target_acc = ctx_inner.uidToAccount.find(uid);
            if (target_acc != ctx_inner.uidToAccount.end()) {
                s2s_res.add_target_account_ids(target_acc->second);
            }
        }

        ctx_inner.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, s2s_res);

        if (target_monster->GetHp() <= 0) {
            std::cout << "[System] 💀 몬스터(ID:" << target_monster->GetId() << ")가 쓰러졌습니다!\n";
            target_monster->Die();
        }
    });
}