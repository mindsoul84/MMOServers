#include "WorldHandlers.h"
#include "../../GameServer.h"
#include "../../Monster/Monster.h" 
#include "../../Network/WorldConnection.h"

#include <iostream>
#include <shared_mutex>
#include <boost/asio/post.hpp>

// World -> Game : 몬스터 버프 지시 처리 핸들러
void Handle_WorldGameMonsterBuff(std::shared_ptr<WorldConnection>& session, char* payload, uint16_t payloadSize) {
    auto req = std::make_shared<Protocol::WorldGameMonsterBuffReq>();

    // 1. 스마트 포인터이므로 '->' 연산자를 사용합니다.
    if (req->ParseFromArray(payload, payloadSize)) {

        auto& ctx = GameContext::Get();

        // 몬스터 상태를 변경하므로 쓰기 락 적용
        std::unique_lock<std::shared_mutex> lock(ctx.gameStateMutex);

        auto& ctx_inner = GameContext::Get();

        uint64_t min_uid = req->min_uid();
        uint64_t max_uid = req->max_uid();
        int32_t add_hp = req->add_hp();

        std::cout << "\n🌟 [S2S 수신] WorldServer로부터 몬스터(" << min_uid << "~" << max_uid
            << ") 체력 버프(+" << add_hp << ") 지시 수신!\n";

        int buff_count = 0;

        // 3. g_monsters 대신 GameContext의 monsters 리스트를 순회합니다.
        for (auto& mon : ctx_inner.monsters) {
            uint64_t uid = mon->GetId();

            // 지정된 범위 안에 있고, 살아있는 몬스터라면 버프 적용!
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
}