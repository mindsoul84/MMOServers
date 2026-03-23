#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <unordered_set> // 빠른 삭제(O(1))를 위해 set 사용

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include <boost/asio.hpp>
#pragma warning(pop)

class StressSession; // 전방 선언

class StressManager {
public:
    StressManager(boost::asio::io_context& io, int target_conn, int spawn_rate);

    void StartStressTest();
    void ScheduleNextSpawn();
    void PrintMetricsLoop();

    void AddRecvCount() { total_packets_recv_.fetch_add(1, std::memory_order_relaxed); }
    void AddSendCount() { total_packets_sent_.fetch_add(1, std::memory_order_relaxed); }

    void OnSessionConnected() { active_connections_.fetch_add(1, std::memory_order_relaxed); }

    // 끊어진 세션 객체를 직접 받아 메모리에서 제거하고 재접속 판별
    void OnSessionDisconnected(std::shared_ptr<StressSession> session, bool was_in_game);

private:
    boost::asio::io_context& io_context_;
    boost::asio::steady_timer ramp_up_timer_;
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
};