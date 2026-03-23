#include "StressManager.h"
#include "../Session/StressSession.h"
#include <iostream>

StressManager::StressManager(boost::asio::io_context& io, int target_conn, int spawn_rate)
    : io_context_(io), ramp_up_timer_(io), metrics_timer_(io),
    target_connections_(target_conn), spawn_rate_per_sec_(spawn_rate) {
}

void StressManager::StartStressTest() {
    std::cout << "🚀 [StressTest] 부하 테스트 시작! 목표 인원: " << target_connections_ << "명\n";
    ScheduleNextSpawn();
    PrintMetricsLoop();
}

void StressManager::ScheduleNextSpawn() {
    std::lock_guard<std::mutex> lock(manager_mtx_); // 타이머 콜백 내 스레드 보호

    int spawn_count = std::min(spawn_rate_per_sec_, target_connections_ - current_spawned_);

    for (int i = 0; i < spawn_count; ++i) {
        // total_spawn_count_를 사용하여 재접속 시에도 항상 고유한 아이디 부여
        std::string bot_id = "BOT_STRESS_" + std::to_string(total_spawn_count_++);
        auto session = std::make_shared<StressSession>(io_context_, this, bot_id);
        sessions_.insert(session); // Set에 삽입
        current_spawned_++;
        session->Start();
    }

    if (current_spawned_ < target_connections_) {
        ramp_up_timer_.expires_after(std::chrono::seconds(1));
        ramp_up_timer_.async_wait([this](boost::system::error_code ec) {
            if (!ec) ScheduleNextSpawn();
            });
    }
}

void StressManager::OnSessionDisconnected(std::shared_ptr<StressSession> session, bool was_in_game) {
    if (was_in_game) {
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(manager_mtx_);
    sessions_.erase(session); // 메모리에서 완벽히 해제 (벡터 누수 해결)
    current_spawned_--;

    // ★ [WARN1 픽스] 봇이 끊어지면 목표 인원 유지를 위해 0.5초 뒤 재접속 시도 (좀비 봇 감소 현상 해결)
    if (current_spawned_ < target_connections_) {
        ramp_up_timer_.expires_after(std::chrono::milliseconds(500));
        ramp_up_timer_.async_wait([this](boost::system::error_code ec) {
            if (!ec) ScheduleNextSpawn();
            });
    }
}

void StressManager::PrintMetricsLoop() {
    metrics_timer_.expires_after(std::chrono::seconds(1));
    metrics_timer_.async_wait([this](boost::system::error_code ec) {
        if (!ec) {
            uint64_t current_recv = total_packets_recv_.load();
            uint64_t tps = current_recv - last_recv_count_;
            last_recv_count_ = current_recv;

            // 명확한 '패킷 수신량'으로 변경
            std::cout << "[Metrics] 🌐 활성 봇: " << active_connections_.load() << "명 | "
                << "📥 패킷 수신: " << tps << " pkt/sec | "
                << "📤 누적 송신: " << total_packets_sent_.load() << "      \r";

            PrintMetricsLoop();
        }
    });
}