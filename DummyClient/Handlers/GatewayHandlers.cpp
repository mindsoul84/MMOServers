#include "GatewayHandlers.h"

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include "protocol.pb.h"
#pragma warning(pop)

#include <iostream>
#include <cmath>

void HandleMoveRes(const std::vector<char>& p, const std::string& my_id, float& my_x, float& my_y, int& my_hp, std::unordered_map<std::string, std::pair<float, float>>& monster_pos_map) {
    Protocol::MoveRes move_res;
    if (move_res.ParseFromArray(p.data(), p.size())) {
        
        if (move_res.account_id() == my_id) {
            float distance = std::sqrt(std::pow(my_x - move_res.x(), 2) + std::pow(my_y - move_res.y(), 2));
            if (distance > 0.1f) {
                my_x = move_res.x();
                my_y = move_res.y();
                if (my_x == 0.0f && my_y == 0.0f && my_hp <= 0) {
                    my_hp = 100;
                    std::cout << "\n✨ [System] 기절하여 서버에 의해 마을로 강제 이동(부활) 되었습니다!\n";
                }
                else {
                    std::cout << "\n🚧 [System] 맵의 경계에 도달하여 위치가 보정되었습니다.\n";
                }
                std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
            }
        }
        else if (move_res.account_id().find("MONSTER_") == 0) {
            std::string mon_id = move_res.account_id();
            float m_x = move_res.x();
            float m_y = move_res.y();

            float dist_to_player = std::sqrt(std::pow(my_x - m_x, 2) + std::pow(my_y - m_y, 2));

            bool is_respawn = false;

            // 넘겨받은 지역 변수 맵(monster_pos_map)을 안전하게 사용합니다.
            if (monster_pos_map.find(mon_id) == monster_pos_map.end()) {
                is_respawn = true;
            }
            else {
                float last_x = monster_pos_map[mon_id].first;
                float last_y = monster_pos_map[mon_id].second;
                float dist_from_last = std::sqrt(std::pow(last_x - m_x, 2) + std::pow(last_y - m_y, 2));

                if (dist_from_last > 2.0f) {
                    is_respawn = true;
                }
            }

            monster_pos_map[mon_id] = { m_x, m_y };

            if (is_respawn && dist_to_player <= 0.1f) {
                std::cout << "\n⚠️ [System] 앗! 당신이 서 있는 좌표(X:" << my_x << ", Y:" << my_y
                    << ")에 " << mon_id << " 가 리스폰(등장)했습니다!\n";
                std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
            }
        }
    }
}

void HandleAttackRes(const std::vector<char>& p, const std::string& my_id, float& my_x, float& my_y, int& my_hp, std::unordered_map<std::string, std::pair<float, float>>& monster_pos_map) {
    Protocol::AttackRes attack_res;
    if (attack_res.ParseFromArray(p.data(), p.size())) {
        if (attack_res.damage() == 0) {
            std::cout << "\n[System] 범위에 벗어나 공격에 실패했습니다.\n";
        }
        // 1. 내가 맞은 경우 (몬스터 -> 나)
        else if (attack_res.target_account_id() == my_id) {
            my_hp = attack_res.target_remain_hp();
            std::cout << "\n🩸 [전투] 몬스터에게 " << attack_res.damage() << " 데미지를 입었습니다!\n";
            if (my_hp <= 0) std::cout << "💀 체력이 0이 되어 기절했습니다...\n";
        }
        // 2. 다른 유저가 맞는 것을 구경하는 경우 (몬스터 -> 다른 유저)
        else if (attack_res.target_account_id().find("MONSTER_") != 0) {
            std::cout << "\n[관전] 🛡️ 다른 유저(" << attack_res.target_account_id() << ")가 몬스터에게 피격당했습니다!\n";
        }
        // 3. 내가(혹은 남이) 몬스터를 때린 경우 (유저 -> 몬스터)
        else {
            std::cout << "\n[Combat] ⚔️ 몬스터(" << attack_res.target_account_id() << ") 타격 성공! 데미지: " << attack_res.damage() << " (남은 체력: " << attack_res.target_remain_hp() << ")\n";
            if (attack_res.target_remain_hp() <= 0) {
                std::cout << "🎉 [System] 💀 몬스터(" << attack_res.target_account_id() << ")가 쓰러졌습니다!\n";
                monster_pos_map.erase(attack_res.target_account_id());
            }
        }
        std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
    }
}