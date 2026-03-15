#pragma once
#include <vector>
#include <string>

// =======================================================
// [Gateway -> Client 패킷 수신 핸들러]
// =======================================================

void HandleMoveRes(const std::vector<char>& p, const std::string& my_id, float& my_x, float& my_y, int& my_hp);
void HandleAttackRes(const std::vector<char>& p, const std::string& my_id, float& my_x, float& my_y, int& my_hp);