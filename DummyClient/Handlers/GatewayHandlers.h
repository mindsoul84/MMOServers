#pragma once
#include <vector>
#include <string>
#include <unordered_map>

// =======================================================
// [Gateway -> Client 패킷 수신 핸들러]
// =======================================================

void HandleMoveRes(const std::vector<char>& p, const std::string& my_id, float& my_x, float& my_y, int& my_hp, std::unordered_map<std::string, std::pair<float, float>>& monster_pos_map);
void HandleAttackRes(const std::vector<char>& p, const std::string& my_id, float& my_x, float& my_y, int& my_hp, std::unordered_map<std::string, std::pair<float, float>>& monster_pos_map);