#include "Zone.h"
#include <iostream>
#include <cmath>

// =========================================================
// Sector 구현부
// =========================================================

void Sector::AddPlayer(uint64_t player_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_); // 쓰기 락
    players_.insert(player_id);
}

void Sector::RemovePlayer(uint64_t player_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_); // 쓰기 락
    players_.erase(player_id);
}

std::vector<uint64_t> Sector::GetPlayers() const {
    std::shared_lock<std::shared_mutex> lock(mutex_); // 읽기 락
    return std::vector<uint64_t>(players_.begin(), players_.end());
}


// =========================================================
// Zone 구현부
// =========================================================

Zone::Zone(int width, int height, int sector_size)
    : width_(width), height_(height), sector_size_(sector_size) {

    // 맵 크기에 맞춰 필요한 격자의 개수 계산
    cols_ = static_cast<int>(std::ceil(static_cast<float>(width_) / sector_size_));
    rows_ = static_cast<int>(std::ceil(static_cast<float>(height_) / sector_size_));

    // 2차원 격자 배열 크기 할당
    //grid_.resize(rows_, std::vector<Sector>(cols_));
    //std::cout << "[Zone] 맵 초기화 완료: " << rows_ << "x" << cols_ << " 격자 생성됨.\n";

    // ★ 2차원 배열 크기 할당 후, 직접 Sector 객체를 동적 생성(make_unique)해서 넣음
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
        //grid_[row][col].AddPlayer(player_id);
        grid_[row][col]->AddPlayer(player_id); // ★ '.' 대신 '->' 사용
    }
}

void Zone::LeaveZone(uint64_t player_id, float x, float y) {
    int row, col;
    // 유저가 마지막으로 서 있던 좌표를 통해 어느 격자(Sector)에 있는지 찾습니다.
    if (GetSectorIndex(x, y, row, col)) {
        grid_[row][col]->RemovePlayer(player_id); // 해당 격자에서 유저 삭제
    }
}

void Zone::UpdatePosition(uint64_t player_id, float old_x, float old_y, float new_x, float new_y) {
    int old_row, old_col, new_row, new_col;

    bool valid_old = GetSectorIndex(old_x, old_y, old_row, old_col);
    bool valid_new = GetSectorIndex(new_x, new_y, new_row, new_col);

    if (valid_old && valid_new) {
        // 다른 섹터로 넘어갔을 때만 빼고 넣기
        if (old_row != new_row || old_col != new_col) {
            //grid_[old_row][old_col].RemovePlayer(player_id);
            //grid_[new_row][new_col].AddPlayer(player_id);
            grid_[old_row][old_col]->RemovePlayer(player_id); // ★ '.' 대신 '->' 사용
            grid_[new_row][new_col]->AddPlayer(player_id);    // ★ '.' 대신 '->' 사용
        }
    }
}

std::vector<uint64_t> Zone::GetPlayersInAOI(float x, float y) const {
    std::vector<uint64_t> aoi_players;
    int center_row, center_col;

    if (!GetSectorIndex(x, y, center_row, center_col)) return aoi_players;

    // 중심 격자를 기준으로 주변 9칸(-1 ~ +1) 탐색
    for (int r = center_row - 1; r <= center_row + 1; ++r) {
        for (int c = center_col - 1; c <= center_col + 1; ++c) {
            // 맵 밖으로 벗어나는 배열 인덱스 방지
            if (r >= 0 && r < rows_ && c >= 0 && c < cols_) {
                //auto players_in_sector = grid_[r][c].GetPlayers();
                auto players_in_sector = grid_[r][c]->GetPlayers(); // ★ '.' 대신 '->' 사용
                
                // 결과 벡터 뒤에 이어 붙이기
                aoi_players.insert(aoi_players.end(), players_in_sector.begin(), players_in_sector.end());
            }
        }
    }
    return aoi_players;
}