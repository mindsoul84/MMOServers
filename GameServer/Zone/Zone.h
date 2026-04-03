#pragma once

#include <vector>
#include <unordered_set>
#include <cstdint>
#include <memory>
#include <functional>
#include <cassert>

#ifndef NDEBUG
#include <atomic>
#endif

// ==========================================
// [AOI 시스템 개선]
//
// 1. 원자적 섹터 전환: Add-then-Remove 순서로 변경
//    변경 전: Remove(old) -> Add(new) -> 사이에 "어디에도 없는" 상태 발생
//    변경 후: Add(new) -> Remove(old) -> 항상 최소 1개 섹터에 존재
//
// 2. 벡터 할당 최적화: reserve()로 사전 용량 확보
//    변경 전: GetPlayersInAOI()가 매번 빈 벡터 생성 후 동적 확장
//    변경 후: 예상 인원수로 reserve() 호출, 불필요한 재할당 감소
//
// 3. 콜백 기반 AOI 조회 추가: ForEachPlayerInAOI
//    벡터 생성 없이 AOI 내 플레이어에 대해 즉시 콜백 실행
//    대규모 브로드캐스트 시 메모리 할당 비용 제거
// ==========================================

// ==========================================
//   Sector 뮤텍스 제거 → 디버그 빌드 동시 접근 감지로 대체
//
// 변경 전: mutable UTILITY::Lock mutex_ 로 읽기/쓰기 보호
//   -> game_strand_가 모든 Zone 접근을 직렬화하고 있어 실질적 경합 없음
//   -> 불필요한 뮤텍스 오버헤드만 발생 (특히 shared_lock 비용)
//
// 변경 후: 뮤텍스 완전 제거
//   -> 디버그 빌드(NDEBUG 미정의)에서만 std::atomic<int> 카운터로
//      동시 접근을 감지하여 strand 직렬화 위반 시 assert 발동
//   -> 릴리즈 빌드에서는 제로 오버헤드 (검증 코드 전부 제거됨)
//
// [전제 조건]
//   Zone의 모든 공개 메서드는 반드시 game_strand_ 컨텍스트에서 호출되어야 함.
//   이 전제가 깨지면 디버그 빌드에서 assert가 발동합니다.
// ==========================================

#ifndef NDEBUG
// 디버그 전용 RAII 동시 접근 감지기
// 동일 Sector에 두 스레드가 동시 진입하면 assert 실패
class SectorAccessGuard {
    std::atomic<int>& counter_;
public:
    explicit SectorAccessGuard(std::atomic<int>& counter) : counter_(counter) {
        int prev = counter_.fetch_add(1, std::memory_order_acquire);
        assert(prev == 0 && "Sector: concurrent access detected! game_strand_ serialization violated");
    }
    ~SectorAccessGuard() {
        counter_.fetch_sub(1, std::memory_order_release);
    }
    SectorAccessGuard(const SectorAccessGuard&) = delete;
    SectorAccessGuard& operator=(const SectorAccessGuard&) = delete;
};
#endif

struct Sector {
#ifndef NDEBUG
    // 디버그 빌드 전용: 동시 접근 횟수 추적 카운터
    // game_strand_가 정상 동작하면 항상 0 또는 1이어야 함
    mutable std::atomic<int> concurrent_access_count_{0};
#endif

    std::unordered_set<uint64_t> players_;
    std::unordered_set<uint64_t> monsters_;

    void AddPlayer(uint64_t player_id);
    void RemovePlayer(uint64_t player_id);
    std::vector<uint64_t> GetPlayers() const;
    size_t GetPlayerCount() const;

    void AddMonster(uint64_t mon_id);
    void RemoveMonster(uint64_t mon_id);
    std::vector<uint64_t> GetMonsters() const;

    // 콜백 기반 조회: 벡터 할당 없이 직접 순회
    template<typename Func>
    void ForEachPlayer(Func&& callback) const {
#ifndef NDEBUG
        SectorAccessGuard guard(concurrent_access_count_);
#endif
        for (uint64_t id : players_) {
            callback(id);
        }
    }

    template<typename Func>
    void ForEachMonster(Func&& callback) const {
#ifndef NDEBUG
        SectorAccessGuard guard(concurrent_access_count_);
#endif
        for (uint64_t id : monsters_) {
            callback(id);
        }
    }
};

class Zone {
private:
    int width_;
    int height_;
    int sector_size_;
    int rows_;
    int cols_;

    std::vector<std::vector<std::unique_ptr<Sector>>> grid_;

public:
    Zone(int width, int height, int sector_size);

    bool GetSectorIndex(float x, float y, int& out_row, int& out_col) const;
    
    void EnterZone(uint64_t player_id, float x, float y);
    void LeaveZone(uint64_t player_id, float x, float y);
    void UpdatePosition(uint64_t player_id, float old_x, float old_y, float new_x, float new_y);
    std::vector<uint64_t> GetPlayersInAOI(float x, float y) const;

    //   콜백 기반 AOI 조회 (벡터 할당 없음)
    template<typename Func>
    void ForEachPlayerInAOI(float x, float y, Func&& callback) const {
        int center_row, center_col;
        if (!GetSectorIndex(x, y, center_row, center_col)) return;

        for (int r = center_row - 1; r <= center_row + 1; ++r) {
            for (int c = center_col - 1; c <= center_col + 1; ++c) {
                if (r >= 0 && r < rows_ && c >= 0 && c < cols_) {
                    grid_[r][c]->ForEachPlayer(callback);
                }
            }
        }
    }

    void EnterZoneMonster(uint64_t mon_id, float x, float y);
    void LeaveZoneMonster(uint64_t mon_id, float x, float y);
    void UpdatePositionMonster(uint64_t mon_id, float old_x, float old_y, float new_x, float new_y);
    std::vector<uint64_t> GetMonstersInAOI(float x, float y) const;

    //   콜백 기반 몬스터 AOI 조회
    template<typename Func>
    void ForEachMonsterInAOI(float x, float y, Func&& callback) const {
        int center_row, center_col;
        if (!GetSectorIndex(x, y, center_row, center_col)) return;

        for (int r = center_row - 1; r <= center_row + 1; ++r) {
            for (int c = center_col - 1; c <= center_col + 1; ++c) {
                if (r >= 0 && r < rows_ && c >= 0 && c < cols_) {
                    grid_[r][c]->ForEachMonster(callback);
                }
            }
        }
    }
};
