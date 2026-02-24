#pragma once // 중복 포함 방지 (현대 C++ 표준 격)

#include <vector>
#include <unordered_set>
#include <shared_mutex>
#include <mutex>
#include <cstdint>
#include <memory> // ★ std::unique_ptr를 사용하기 위해 추가

// ---------------------------------------------------------
// 1. Sector (격자 1칸) 구조체 선언
// ---------------------------------------------------------
struct Sector {
    mutable std::shared_mutex mutex_;
    std::unordered_set<uint64_t> players_;

    void AddPlayer(uint64_t player_id);
    void RemovePlayer(uint64_t player_id);
    std::vector<uint64_t> GetPlayers() const;
};

// ---------------------------------------------------------
// 2. Zone (맵 전체 관리) 클래스 선언
// ---------------------------------------------------------
class Zone {
private:
    int width_;         // 맵 전체 가로 크기
    int height_;        // 맵 전체 세로 크기
    int sector_size_;   // 격자 한 칸의 크기
    int rows_;          // 생성된 행 개수
    int cols_;          // 생성된 열 개수

    //std::vector<std::vector<Sector>> grid_;
    // ★ Sector 객체가 아닌, Sector의 스마트 포인터를 저장하도록 변경
    std::vector<std::vector<std::unique_ptr<Sector>>> grid_;

public:
    // 생성자
    Zone(int width, int height, int sector_size);

    // 좌표를 격자 인덱스로 변환
    bool GetSectorIndex(float x, float y, int& out_row, int& out_col) const;

    // 유저 입장
    void EnterZone(uint64_t player_id, float x, float y);

    // 유저 퇴장
    void LeaveZone(uint64_t player_id, float x, float y);

    // 유저 이동 시 섹터 갱신
    void UpdatePosition(uint64_t player_id, float old_x, float old_y, float new_x, float new_y);

    // 내 주변(AOI) 유저 목록 가져오기
    std::vector<uint64_t> GetPlayersInAOI(float x, float y) const;
};