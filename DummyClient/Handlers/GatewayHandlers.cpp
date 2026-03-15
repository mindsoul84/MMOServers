#include "GatewayHandlers.h"
#include "protocol.pb.h"
#include <iostream>
#include <cmath>

void HandleMoveRes(const std::vector<char>& p, const std::string& my_id, float& my_x, float& my_y, int& my_hp) {
    Protocol::MoveRes move_res;
    if (move_res.ParseFromArray(p.data(), p.size()) && move_res.account_id() == my_id) {
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
}

void HandleAttackRes(const std::vector<char>& p, const std::string& my_id, float& my_x, float& my_y, int& my_hp) {
    Protocol::AttackRes attack_res;
    if (attack_res.ParseFromArray(p.data(), p.size())) {
        if (attack_res.damage() == 0) {
            std::cout << "\n[System] 범위에 벗어나 공격에 실패했습니다.\n";
        }
        else if (attack_res.target_account_id() == my_id) {
            my_hp = attack_res.target_remain_hp();
            std::cout << "\n🩸 [전투] 몬스터에게 " << attack_res.damage() << " 데미지를 입었습니다!\n";
            if (my_hp <= 0) std::cout << "💀 체력이 0이 되어 기절했습니다...\n";
        }
        else {
            std::cout << "\n[Combat] ⚔️ 몬스터(" << attack_res.target_account_id() << ") 타격 성공! 데미지: " << attack_res.damage() << " (남은 체력: " << attack_res.target_remain_hp() << ")\n";
            if (attack_res.target_remain_hp() <= 0) {
                std::cout << "🎉 [System] 💀 몬스터(" << attack_res.target_account_id() << ")가 쓰러졌습니다!\n";
            }
        }
        std::cout << "[내 정보] HP: " << my_hp << " | 위치 X:" << my_x << " Y:" << my_y << "          \r";
    }
}