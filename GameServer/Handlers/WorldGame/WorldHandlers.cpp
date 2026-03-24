#include "WorldHandlers.h"
#include "../../GameServer.h"
#include "../../Monster/Monster.h"
#include "../../Network/WorldConnection.h"

#include <iostream>
#include <shared_mutex>
#include <boost/asio/post.hpp>

void Handle_WorldGameMonsterBuff(std::shared_ptr<WorldConnection>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::WorldGameMonsterBuffReq>();

    // ★ [수정 2] ParseFromArray 실패 시 로그 출력
    if (!req->ParseFromArray(payload, payloadSize)) {
        std::cerr << "[GameServer] 🚨 ParseFromArray 실패: WorldGameMonsterBuffReq (payloadSize=" << payloadSize << ")\n";
        return;
    }

    auto& ctx = GameContext::Get();

    std::unique_lock<std::shared_mutex> lock(ctx.gameStateMutex);

    uint64_t min_uid = req->min_uid();
    uint64_t max_uid = req->max_uid();
    int32_t add_hp   = req->add_hp();

    std::cout << "\n🌟 [S2S 수신] WorldServer로부터 몬스터(" << min_uid << "~" << max_uid
        << ") 체력 버프(+" << add_hp << ") 지시 수신!\n";

    int buff_count = 0;

    for (auto& mon : ctx.monsters) {
        uint64_t uid = mon->GetId();
        if (uid >= min_uid && uid <= max_uid) {
            if (mon->GetState() != MonsterState::DEAD) {
                mon->SetHp(add_hp);
                buff_count++;
                std::cout << "  -> 몬스터[" << uid << "] 체력 증가! (현재 HP: " << mon->GetHp() << ")\n";
            }
        }
    }
    std::cout << "▶ 총 " << buff_count << "마리의 몬스터에게 버프가 적용되었습니다.\n";
}
