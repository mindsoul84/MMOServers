#include "WorldHandlers.h"
#include "../../GameServer.h"
#include "../../Monster/Monster.h"
#include "../../Network/WorldConnection.h"
#include "../../../Common/Utils/Logger.h"

#include <iostream>
#include <shared_mutex>
#include <boost/asio/post.hpp>

void Handle_WorldGameMonsterBuff(std::shared_ptr<WorldConnection>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::WorldGameMonsterBuffReq>();

    if (!req->ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("GameServer", "ParseFromArray 실패: WorldGameMonsterBuffReq (payloadSize=" << payloadSize << ")");
        return;
    }

    auto& ctx = GameContext::Get();

    // [수정] UTILITY::WriteLock 타입으로 통일 — monsterMutex_ 쓰기 락: 몬스터 체력 변경
    UTILITY::WriteLock w_lock(ctx.monsterMutex_);

    uint64_t min_uid = req->min_uid();
    uint64_t max_uid = req->max_uid();
    int32_t add_hp   = req->add_hp();

    LOG_INFO("S2S", "WorldServer로부터 몬스터(" << min_uid << "~" << max_uid
        << ") 체력 버프(+" << add_hp << ") 지시 수신!");

    int buff_count = 0;

    for (auto& mon : ctx.monsters) {
        uint64_t uid = mon->GetId();
        if (uid >= min_uid && uid <= max_uid) {
            if (mon->GetState() != MonsterState::DEAD) {
                mon->SetHp(add_hp);
                buff_count++;
            }
        }
    }
    LOG_INFO("S2S", "총 " << buff_count << "마리의 몬스터에게 버프가 적용되었습니다.");
}

// ==========================================
// [추가] WorldServer -> GameServer 토큰 통지 수신
//
// WorldServer에서 유저에게 토큰을 발급한 뒤 이 핸들러로 통지합니다.
// GameServer는 연결된 모든 GatewaySession에 동일한 토큰을 전달(중계)합니다.
// GatewayServer는 이 토큰을 pending_tokens에 저장하여 클라이언트 접속 시 검증합니다.
// ==========================================
void Handle_WorldGameTokenNotify(std::shared_ptr<WorldConnection>& session, char* payload, uint16_t payloadSize) {
    Protocol::TokenNotify notify;
    if (!notify.ParseFromArray(payload, payloadSize)) {
        LOG_ERROR("GameServer", "ParseFromArray 실패: TokenNotify (payloadSize=" << payloadSize << ")");
        return;
    }

    LOG_INFO("GameServer", "토큰 통지 수신 → GatewayServer로 중계 (유저: " << notify.account_id() << ")");

    // 연결된 모든 GatewaySession에 토큰을 중계
    auto& ctx = GameContext::Get();
    ctx.BroadcastToGateways(Protocol::PKT_GAME_GATEWAY_TOKEN_NOTIFY, notify);
}
