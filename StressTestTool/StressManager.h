#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

#pragma warning(push)
#pragma warning(disable: 26495 26439 26451 26812 26815 26816 6385 6386 6001 6255 6387 6031 6258 26819 26498)
#include <boost/asio.hpp>
#pragma warning(pop)

#include "StressSession.h"

class StressManager {
private:
    boost::asio::io_context& io_context_;
    boost::asio::steady_timer ramp_up_timer_;
    boost::asio::steady_timer metrics_timer_;

    std::vector<std::shared_ptr<StressSession>> sessions_;

    int target_connections_;
    int spawn_rate_per_sec_;
    int current_spawned_ = 0;

    // ★ 성능 지표 (원자적 연산으로 멀티스레드 보호)
    std::atomic<int> active_connections_{ 0 };
    std::atomic<int> total_packets_sent_{ 0 };
    std::atomic<int> total_packets_recv_{ 0 };

    int last_recv_count_ = 0;

public:
    StressManager(boost::asio::io_context& io, int target_conn, int spawn_rate);

    void StartStressTest();

    // 세션이 매니저에게 지표를 보고할 때 사용
    void OnSessionConnected() { active_connections_++; }
    void OnSessionDisconnected() { active_connections_--; }
    void AddSendCount() { total_packets_sent_++; }
    void AddRecvCount() { total_packets_recv_++; }

private:
    void ScheduleNextSpawn();
    void PrintMetricsLoop();
};