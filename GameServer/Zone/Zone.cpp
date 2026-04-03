#include "Zone.h"
#include <iostream>
#include <cmath>

// =========================================================
// Sector 구현부
//   뮤텍스 제거 → 디버그 빌드 동시 접근 감지로 대체
//
// game_strand_가 모든 게임 로직(패킷 핸들러, AI Tick, 경로 콜백)을
// 직렬화하고 있으므로, Sector에 대한 동시 접근이 원천적으로 불가능합니다.
// 디버그 빌드에서 SectorAccessGuard가 이 불변 조건을 런타임으로 검증합니다.
// =========================================================

void Sector::AddPlayer(uint64_t player_id) {
#ifndef NDEBUG
    SectorAccessGuard guard(concurrent_access_count_);
#endif
    players_.insert(player_id);
}

void Sector::RemovePlayer(uint64_t player_id) {
#ifndef NDEBUG
    SectorAccessGuard guard(concurrent_access_count_);
#endif
    players_.erase(player_id);
}

std::vector<uint64_t> Sector::GetPlayers() const {
#ifndef NDEBUG
    SectorAccessGuard guard(concurrent_access_count_);
#endif
    return std::vector<uint64_t>(players_.begin(), players_.end());
}

size_t Sector::GetPlayerCount() const {
#ifndef NDEBUG
    SectorAccessGuard guard(concurrent_access_count_);
#endif
    return players_.size();
}

void Sector::AddMonster(uint64_t mon_id) {
#ifndef NDEBUG
    SectorAccessGuard guard(concurrent_access_count_);
#endif
    monsters_.insert(mon_id);
}

void Sector::RemoveMonster(uint64_t mon_id) {
#ifndef NDEBUG
    SectorAccessGuard guard(concurrent_access_count_);
#endif
    monsters_.erase(mon_id);
}

std::vector<uint64_t> Sector::GetMonsters() const {
#ifndef NDEBUG
    SectorAccessGuard guard(concurrent_access_count_);
#endif
    return std::vector<uint64_t>(monsters_.begin(), monsters_.end());
}


// =========================================================
// Zone 구현부
// =========================================================

Zone::Zone(int width, int height, int sector_size)
    : width_(width), height_(height), sector_size_(sector_size) {

    cols_ = static_cast<int>(std::ceil(static_cast<float>(width_) / sector_size_));
    rows_ = static_cast<int>(std::ceil(static_cast<float>(height_) / sector_size_));

    grid_.resize(rows_);
    for (int r = 0; r < rows_; ++r) {
        for (int c = 0; c < cols_; ++c) {
            grid_[r].push_back(std::make_unique<Sector>());
        }
    }
    std::cout << "[Zone] 맵 초기화 완료: " << rows_ << "x" << cols_ << " 격자 생성됨.\n";
}

bool Zone::GetSectorIndex(float x, float y, int& out_row, int& out_col) const {
    if (x < 0 || x >= width_ || y < 0 || y >= height_) return false;

    out_col = static_cast<int>(x / sector_size_);
    out_row = static_cast<int>(y / sector_size_);
    return true;
}

void Zone::EnterZone(uint64_t player_id, float x, float y) {
    int row, col;
    if (GetSectorIndex(x, y, row, col)) {
        grid_[row][col]->AddPlayer(player_id);
    }
}

void Zone::LeaveZone(uint64_t player_id, float x, float y) {
    int row, col;
    if (GetSectorIndex(x, y, row, col)) {
        grid_[row][col]->RemovePlayer(player_id);
    }
}

// ==========================================
// [원자적 섹터 전환] Add-then-Remove 패턴
// ==========================================
void Zone::UpdatePosition(uint64_t player_id, float old_x, float old_y, float new_x, float new_y) {
    int old_row, old_col, new_row, new_col;

    bool valid_old = GetSectorIndex(old_x, old_y, old_row, old_col);
    bool valid_new = GetSectorIndex(new_x, new_y, new_row, new_col);

    if (valid_old && valid_new) {
        if (old_row != new_row || old_col != new_col) {
            // [핵심] 새 섹터에 먼저 추가, 이후 이전 섹터에서 제거
            grid_[new_row][new_col]->AddPlayer(player_id);
            grid_[old_row][old_col]->RemovePlayer(player_id);
        }
    }
    else if (valid_new) {
        grid_[new_row][new_col]->AddPlayer(player_id);
    }
}

// ==========================================
// [벡터 할당 최적화] reserve()로 사전 용량 확보
// ==========================================
std::vector<uint64_t> Zone::GetPlayersInAOI(float x, float y) const {
    std::vector<uint64_t> aoi_players;
    int center_row, center_col;

    if (!GetSectorIndex(x, y, center_row, center_col)) return aoi_players;

    // 예상 인원수 추정 후 reserve (최대 9개 섹터)
    size_t estimated_count = 0;
    for (int r = center_row - 1; r <= center_row + 1; ++r) {
        for (int c = center_col - 1; c <= center_col + 1; ++c) {
            if (r >= 0 && r < rows_ && c >= 0 && c < cols_) {
                estimated_count += grid_[r][c]->GetPlayerCount();
            }
        }
    }
    aoi_players.reserve(estimated_count);

    for (int r = center_row - 1; r <= center_row + 1; ++r) {
        for (int c = center_col - 1; c <= center_col + 1; ++c) {
            if (r >= 0 && r < rows_ && c >= 0 && c < cols_) {
                auto players_in_sector = grid_[r][c]->GetPlayers();
                aoi_players.insert(aoi_players.end(), players_in_sector.begin(), players_in_sector.end());
            }
        }
    }
    return aoi_players;
}

// 몬스터 Zone 관리: 동일한 Add-then-Remove 패턴 적용
void Zone::EnterZoneMonster(uint64_t mon_id, float x, float y) {
    int row, col;
    if (GetSectorIndex(x, y, row, col)) grid_[row][col]->AddMonster(mon_id);
}

void Zone::LeaveZoneMonster(uint64_t mon_id, float x, float y) {
    int row, col;
    if (GetSectorIndex(x, y, row, col)) grid_[row][col]->RemoveMonster(mon_id);
}

void Zone::UpdatePositionMonster(uint64_t mon_id, float old_x, float old_y, float new_x, float new_y) {
    int old_row, old_col, new_row, new_col;
    if (GetSectorIndex(old_x, old_y, old_row, old_col) && GetSectorIndex(new_x, new_y, new_row, new_col)) {
        if (old_row != new_row || old_col != new_col) {
            grid_[new_row][new_col]->AddMonster(mon_id);
            grid_[old_row][old_col]->RemoveMonster(mon_id);
        }
    }
}

std::vector<uint64_t> Zone::GetMonstersInAOI(float x, float y) const {
    std::vector<uint64_t> aoi_monsters;
    int center_row, center_col;
    if (!GetSectorIndex(x, y, center_row, center_col)) return aoi_monsters;

    aoi_monsters.reserve(16);

    for (int r = center_row - 1; r <= center_row + 1; ++r) {
        for (int c = center_col - 1; c <= center_col + 1; ++c) {
            if (r >= 0 && r < rows_ && c >= 0 && c < cols_) {
                auto mons_in_sector = grid_[r][c]->GetMonsters();
                aoi_monsters.insert(aoi_monsters.end(), mons_in_sector.begin(), mons_in_sector.end());
            }
        }
    }
    return aoi_monsters;
}
