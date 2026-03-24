#include "GatewayHandlers.h"
#include "../../GameServer.h"
#include "../../Session/GatewaySession.h"
#include "../../Monster/Monster.h" // 몬스터 공격 처리를 위해 추가
#include "../../../Common/Define/Define_Server.h"
#include "../../../Common/Define/GameConstants.h"  // ★ [추가] 게임 상수

#include <iostream>
#include <boost/asio/post.hpp>
#include <cmath> // std::sqrt
#include <shared_mutex>

// [게이트웨이 -> 게임서버] 유저 이동 처리
#ifdef  DEF_STRESS_TEST_SERVER
void Handle_GatewayGameMoveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {

    auto req = std::make_shared<Protocol::GatewayGameMoveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) { std::cerr << "[GameServer] 🚨 ParseFromArray 실패: " << __func__ << " (payloadSize=" << payloadSize << ")\n"; return; }

    auto& ctx = GameContext::Get();

    std::string acc_id = req->account_id();
    float new_x = req->x();
    float new_y = req->y();

    // 맵 이탈 방지 (상수 사용)
    if (new_x < 0.0f) new_x = 0.0f;
    if (new_y < 0.0f) new_y = 0.0f;
    if (new_x > GameConstants::Map::WIDTH) new_x = GameConstants::Map::WIDTH;
    if (new_y > GameConstants::Map::HEIGHT) new_y = GameConstants::Map::HEIGHT;

    std::shared_ptr<PlayerInfo> player_ptr;

    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex);
        auto it = ctx.playerMap.find(acc_id);
        if (it != ctx.playerMap.end()) player_ptr = it->second;
    }

    // =========================================================
    // EnterZone 호출 전 gameStateMutex 락 해제
    // =========================================================
    if (!player_ptr) {
        bool is_new = false;
        uint64_t new_uid = 0;

        {
            std::unique_lock<std::shared_mutex> write_lock(ctx.gameStateMutex);
            auto it = ctx.playerMap.find(acc_id);
            if (it == ctx.playerMap.end()) {
                new_uid = ctx.uidCounter++;
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
        } // <--- 여기서 gameStateMutex 쓰기 락이 완전히 풀립니다!

        // 자물쇠가 없는 안전한 상태에서 Zone에 진입합니다.
        if (is_new) {
#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
            if (acc_id.find("BOT_STRESS") != std::string::npos) {
                ctx.connected_bot_count.fetch_add(1, std::memory_order_relaxed);      // 패킷을 하나 처리할 때마다 카운터 증가
            }
#endif//DEF_STRESS_TEST_DEADLOCK_WATCHDOG
            ctx.zone->EnterZone(new_uid, new_x, new_y);
            std::cout << "[GameServer] 유저(" << acc_id << ") 최초 Zone 진입 (UID:" << new_uid << ")\n";
        }
    }

    float old_x = 0.0f;
    float old_y = 0.0f;
    {
        std::lock_guard<std::mutex> p_lock(player_ptr->mtx);
        old_x = player_ptr->x;
        old_y = player_ptr->y;

        player_ptr->x = new_x;
        player_ptr->y = new_y;
    }
    ctx.zone->UpdatePosition(player_ptr->uid, old_x, old_y, new_x, new_y);

    auto aoi_uids = ctx.zone->GetPlayersInAOI(new_x, new_y);
    Protocol::GameGatewayMoveRes s2s_res;
    s2s_res.set_account_id(acc_id);
    s2s_res.set_x(new_x);
    s2s_res.set_y(new_y);

    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex);

        int broadcast_limit = 0;

        for (uint64_t target_uid : aoi_uids)
        {
            if (broadcast_limit++ >= GameConstants::Network::MAX_AOI_BROADCAST) break;
            auto it = ctx.uidToAccount.find(target_uid);
            if (it != ctx.uidToAccount.end())
            {
                s2s_res.add_target_account_ids(it->second);
            }
        }
    }
    
    session->Send(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
}

#else //DEF_STRESS_TEST_SERVER
void Handle_GatewayGameMoveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::GatewayGameMoveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) { std::cerr << "[GameServer] 🚨 ParseFromArray 실패: " << __func__ << " (payloadSize=" << payloadSize << ")\n"; return; }

    auto& ctx = GameContext::Get();

    std::string acc_id = req->account_id();
    float new_x = req->x();
    float new_y = req->y();

    // =========================================================
    // ★ [버그 픽스] 맵 이탈(음수 좌표 및 최대 크기 초과) 방지 로직 
    // 맵 크기 밖으로 나가려고 하면 강제로 벽에 붙여버립니다.
    // =========================================================
    if (new_x < 0.0f) new_x = 0.0f;
    if (new_y < 0.0f) new_y = 0.0f;
    if (new_x > GameConstants::Map::WIDTH) new_x = GameConstants::Map::WIDTH;
    if (new_y > GameConstants::Map::HEIGHT) new_y = GameConstants::Map::HEIGHT;
    // =========================================================

    std::shared_ptr<PlayerInfo> player_ptr;

    // =========================================================
    // ★ 1단계: 글로벌 읽기 락(shared_lock)으로 유저가 있는지 확인만 함 (병목 0%)
    // =========================================================
    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex);
        auto it = ctx.playerMap.find(acc_id);
        if (it != ctx.playerMap.end()) player_ptr = it->second;
    }

    // =========================================================
    // ★ 2단계: 유저가 없다면 신규 진입이므로 이때만 짧게 글로벌 쓰기 락! (Double-checked)
    // =========================================================
    if (!player_ptr) {
        std::unique_lock<std::shared_mutex> write_lock(ctx.gameStateMutex);
        auto it = ctx.playerMap.find(acc_id); // 혹시 그 찰나에 남이 넣었나 다시 검사
        if (it == ctx.playerMap.end()) {
            uint64_t new_uid = ctx.uidCounter++;
            player_ptr = std::make_shared<PlayerInfo>(); // ★ 포인터 할당
            player_ptr->uid = new_uid;
            player_ptr->x = new_x;
            player_ptr->y = new_y;

            ctx.playerMap[acc_id] = player_ptr;
            ctx.uidToAccount[new_uid] = acc_id;
            ctx.zone->EnterZone(new_uid, new_x, new_y);
            std::cout << "[GameServer] 유저(" << acc_id << ") 최초 Zone 진입 (UID:" << new_uid << ")\n";
        }
        else {
            player_ptr = it->second;
        }
    }

    // =========================================================
    // ★ 3단계: 이미 있는 유저의 좌표 갱신은 '유저 개인의 락'만 잡고 실행!
    // 남들은 자유롭게 서버를 이용할 수 있습니다.
    // =========================================================
    {
        std::lock_guard<std::mutex> p_lock(player_ptr->mtx);
        ctx.zone->UpdatePosition(player_ptr->uid, player_ptr->x, player_ptr->y, new_x, new_y);
        player_ptr->x = new_x;
        player_ptr->y = new_y;
    }

    auto aoi_uids = ctx.zone->GetPlayersInAOI(new_x, new_y);
    Protocol::GameGatewayMoveRes s2s_res;
    s2s_res.set_account_id(acc_id);
    s2s_res.set_x(new_x);
    s2s_res.set_y(new_y);

    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex); // ★ 아이디 조회용 짧은 읽기 락
        for (uint64_t target_uid : aoi_uids) {
            auto it = ctx.uidToAccount.find(target_uid);
            if (it != ctx.uidToAccount.end()) {
                s2s_res.add_target_account_ids(it->second);
            }
        }
    }
    session->Send(Protocol::PKT_GAME_GATEWAY_MOVE_RES, s2s_res);
}
#endif//DEF_STRESS_TEST_SERVER



#ifdef  DEF_STRESS_TEST_SERVER
// [게이트웨이 -> 게임서버] 유저 퇴장 처리 핸들러
void Handle_GatewayGameLeaveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::GatewayGameLeaveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) { std::cerr << "[GameServer] 🚨 ParseFromArray 실패: " << __func__ << " (payloadSize=" << payloadSize << ")\n"; return; }

    auto& ctx = GameContext::Get();
    std::string acc_id = req->account_id();

    // =========================================================
    // LeaveZone 호출 전 gameStateMutex 락 해제
    // =========================================================
    uint64_t uid = 0;
    float last_x = 0.0f;
    float last_y = 0.0f;
    bool found = false;

    {
        std::unique_lock<std::shared_mutex> lock(ctx.gameStateMutex);
        auto it = ctx.playerMap.find(acc_id);
        if (it != ctx.playerMap.end()) {
            uid = it->second->uid;
            last_x = it->second->x;
            last_y = it->second->y;

            ctx.uidToAccount.erase(uid);
            ctx.playerMap.erase(it);
            found = true;
        }
    } // <--- 여기서 gameStateMutex 쓰기 락이 완전히 풀립니다!

    // 자물쇠가 없는 안전한 상태에서 Zone에서 퇴장합니다.
    if (found) {

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
        if (acc_id.find("BOT_STRESS") != std::string::npos) {
            ctx.connected_bot_count.fetch_sub(1, std::memory_order_relaxed);
        }
#endif//DEF_STRESS_TEST_DEADLOCK_WATCHDOG

        ctx.zone->LeaveZone(uid, last_x, last_y);
        std::cout << "[GameServer] 👻 유저(" << acc_id << ", UID:" << uid << ") 퇴장 완료. Zone에서 삭제됨.\n";
    }
}
#else //DEF_STRESS_TEST_SERVER
// [게이트웨이 -> 게임서버] 유저 퇴장 처리 핸들러 (맵 삭제가 일어나므로 쓰기 락 유지)
void Handle_GatewayGameLeaveReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::GatewayGameLeaveReq>();
    if (!req->ParseFromArray(payload, payloadSize)) { std::cerr << "[GameServer] 🚨 ParseFromArray 실패: " << __func__ << " (payloadSize=" << payloadSize << ")\n"; return; }

    auto& ctx = GameContext::Get();

    // 쓰기 락 적용
    std::unique_lock<std::shared_mutex> lock(ctx.gameStateMutex);

    std::string acc_id = req->account_id();
    auto it = ctx.playerMap.find(acc_id);
    if (it != ctx.playerMap.end()) {
        uint64_t uid = it->second->uid;
        float last_x = it->second->x;
        float last_y = it->second->y;

        ctx.zone->LeaveZone(uid, last_x, last_y);
        ctx.uidToAccount.erase(uid);
        ctx.playerMap.erase(it);

        std::cout << "[GameServer] 👻 유저(" << acc_id << ", UID:" << uid << ") 퇴장 완료. Zone에서 삭제됨.\n";
    }
}
#endif//DEF_STRESS_TEST_SERVER

#ifdef  DEF_STRESS_TEST_SERVER
// [게이트웨이 -> 게임서버] 유저의 공격 요청 처리
void Handle_GatewayGameAttackReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t size) {

    auto req = std::make_shared<Protocol::GatewayGameAttackReq>();
    if (!req->ParseFromArray(payload, size)) { std::cerr << "[GameServer] 🚨 ParseFromArray 실패: " << __func__ << " (payloadSize=" << size << ")\n"; return; }

    auto& ctx = GameContext::Get();
    std::string account_id = req->account_id();
    std::shared_ptr<PlayerInfo> player_ptr;

#ifdef  DEF_STRESS_TEST_DEADLOCK_WATCHDOG
    if (account_id.find("BOT_STRESS") != std::string::npos) {
        ctx.processed_packet_count.fetch_add(1, std::memory_order_relaxed);      // 패킷을 하나 처리할 때마다 카운터 증가
    }
#endif//DEF_STRESS_TEST_DEADLOCK_WATCHDOG

    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex);
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

    // =========================================================
    // TakeDamage 호출 전 gameStateMutex 락 해제
    // =========================================================
    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex);
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
    } // <--- 여기서 gameStateMutex 읽기 락이 완전히 풀립니다!

    // 사거리 내에 몬스터가 없을 경우
    if (!target_monster) {
        Protocol::GameGatewayAttackRes fail_res;
        fail_res.set_attacker_uid(p_uid);
        fail_res.set_damage(0);
        fail_res.add_target_account_ids(account_id);
        ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, fail_res);
        return;
    }

    // 자물쇠가 없는 안전한 상태에서 데미지 연산을 수행합니다!
    int damage = p_atk - target_monster->GetDef();
    if (damage < 1) damage = 1;

    int remain_hp = target_monster->TakeDamage(damage);

    std::cout << "[Combat] ⚔️ 유저(" << account_id << ")가 몬스터(ID:"
        << target_monster->GetId() << ") 공격! 데미지: " << damage << "\n";

    Protocol::GameGatewayAttackRes s2s_res;
    s2s_res.set_attacker_uid(p_uid);
    s2s_res.set_target_uid(target_monster->GetId());
    s2s_res.set_target_account_id("MONSTER_" + std::to_string(target_monster->GetId()));
    s2s_res.set_damage(damage);
    s2s_res.set_target_remain_hp(remain_hp);

    auto aoi_uids = ctx.zone->GetPlayersInAOI(p_x, p_y);
    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex);

        int broadcast_limit = 0;
        for (uint64_t uid : aoi_uids)
        {
            if (broadcast_limit++ >= GameConstants::Network::MAX_AOI_BROADCAST) break;
            auto target_acc = ctx.uidToAccount.find(uid);
            if (target_acc != ctx.uidToAccount.end()) {
                s2s_res.add_target_account_ids(target_acc->second);
            }
        }
    }

    ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, s2s_res);

    if (remain_hp <= 0)
    {
        std::cout << "[System] 💀 몬스터(ID:" << target_monster->GetId() << ")가 쓰러졌습니다!\n";
        target_monster->Die();
    }
}
#else //DEF_STRESS_TEST_SERVER
// [게이트웨이 -> 게임서버] 유저의 공격 요청 처리
void Handle_GatewayGameAttackReq(std::shared_ptr<GatewaySession>& session, char* payload, uint16_t size) {
    auto req = std::make_shared<Protocol::GatewayGameAttackReq>();
    if (!req->ParseFromArray(payload, size)) { std::cerr << "[GameServer] 🚨 ParseFromArray 실패: " << __func__ << " (payloadSize=" << size << ")\n"; return; }

    auto& ctx = GameContext::Get();

    std::string account_id = req->account_id();
    std::shared_ptr<PlayerInfo> player_ptr;

    // ★ 유저 존재 확인 (읽기 락)
    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex);
        auto it_player = ctx.playerMap.find(account_id);
        if (it_player == ctx.playerMap.end()) return;
        player_ptr = it_player->second;
    }

    // ★ 내 정보 복사 (내 자물쇠 걸고 안전하게 빼옴)
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

    // 타겟 몬스터 탐색
    std::shared_ptr<Monster> target_monster = nullptr;
    float min_dist = GameConstants::Combat::PLAYER_ATTACK_RANGE;

    // =========================================================
    // 전체 순회(O(N)) 제거! Zone 기반 O(1) 탐색!
    // =========================================================
    auto aoi_mon_ids = ctx.zone->GetMonstersInAOI(p_x, p_y);

    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex);
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

    // 데미지 연산 및 적용
    int damage = p_atk - target_monster->GetDef();
    if (damage < 1) damage = 1;

    int remain_hp = target_monster->TakeDamage(damage);

    std::cout << "[Combat] ⚔️ 유저(" << account_id << ")가 몬스터(ID:"
        << target_monster->GetId() << ") 공격! 데미지: " << damage << "\n";

    // 주변 유저에게 전투 결과 브로드캐스트
    Protocol::GameGatewayAttackRes s2s_res;
    s2s_res.set_attacker_uid(p_uid);
    s2s_res.set_target_uid(target_monster->GetId());
    s2s_res.set_target_account_id("MONSTER_" + std::to_string(target_monster->GetId()));
    s2s_res.set_damage(damage);
    s2s_res.set_target_remain_hp(remain_hp);

    auto aoi_uids = ctx.zone->GetPlayersInAOI(p_x, p_y);
    {
        std::shared_lock<std::shared_mutex> read_lock(ctx.gameStateMutex);

        for (uint64_t uid : aoi_uids)
        {
            auto target_acc = ctx.uidToAccount.find(uid);
            if (target_acc != ctx.uidToAccount.end()) {
                s2s_res.add_target_account_ids(target_acc->second);
            }
        }
    }

    ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_ATTACK_RES, s2s_res);

    if (remain_hp <= 0) {
        std::cout << "[System] 💀 몬스터(ID:" << target_monster->GetId() << ")가 쓰러졌습니다!\n";
        target_monster->Die();
    }
}
#endif//DEF_STRESS_TEST_SERVER