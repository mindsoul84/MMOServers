#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_set>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include <boost/asio.hpp>
#pragma warning(pop)

class StressSession;

// ==========================================
// [수정] 타이머 분리로 봇 재접속 안정성 확보
//
// [문제]
// ramp_up_timer_ 하나로 초기 스폰과 재접속을 모두 처리하고 있었음.
// 여러 봇이 거의 동시에 끊어질 때 OnSessionDisconnected가 호출되며
// ramp_up_timer_.expires_after(500ms)가 반복 호출됨.
// expires_after는 이전 대기를 취소하므로, 10개 봇이 100ms 간격으로 끊어지면
// 앞선 9개의 재스폰 요청이 모두 취소되어 봇 보충이 지연됨.
//
// [수정]
// 1. ramp_up_timer_: 초기 3000명 스폰 전용 (기존 역할 유지)
// 2. reconnect_timer_: 1초 주기로 current_spawned_ < target 여부를 점검하여
//    부족분만큼 재스폰하는 독립 유지보수 루프 (신규 추가)
// 3. OnSessionDisconnected에서 타이머를 건드리지 않음 (타이머 취소 문제 근본 제거)
// ==========================================
class StressManager {
public:
    StressManager(boost::asio::io_context& io, int target_conn, int spawn_rate);

    void StartStressTest();
    void ScheduleNextSpawn();
    void PrintMetricsLoop();

    void AddRecvCount() { total_packets_recv_.fetch_add(1, std::memory_order_relaxed); }
    void AddSendCount() { total_packets_sent_.fetch_add(1, std::memory_order_relaxed); }

    void OnSessionConnected() { active_connections_.fetch_add(1, std::memory_order_relaxed); }

    void OnSessionDisconnected(std::shared_ptr<StressSession> session, bool was_in_game);

private:
    boost::asio::io_context& io_context_;
    boost::asio::steady_timer ramp_up_timer_;     // 초기 스폰 전용
    boost::asio::steady_timer reconnect_timer_;   // [추가] 재접속 유지보수 루프 전용
    boost::asio::steady_timer metrics_timer_;

    int target_connections_;
    int spawn_rate_per_sec_;
    int current_spawned_ = 0;
    uint64_t total_spawn_count_ = 0; // 재접속 봇의 이름 충돌을 막기 위한 누적 카운터

    std::mutex manager_mtx_;                                        // 스레드 경합 방지용 뮤텍스
    std::unordered_set<std::shared_ptr<StressSession>> sessions_;   // 벡터 누수 방지

    std::atomic<int> active_connections_{ 0 };
    std::atomic<uint64_t> total_packets_recv_{ 0 };
    std::atomic<uint64_t> total_packets_sent_{ 0 };
    uint64_t last_recv_count_ = 0;

    // [추가] 독립 재접속 유지보수 루프
    void StartReconnectLoop();
};
