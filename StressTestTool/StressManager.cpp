#include "StressManager.h"
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
    int spawn_count = std::min(spawn_rate_per_sec_, target_connections_ - current_spawned_);

    for (int i = 0; i < spawn_count; ++i) {
        std::string bot_id = "BOT_STRESS_" + std::to_string(current_spawned_++);
        auto session = std::make_shared<StressSession>(io_context_, this, bot_id);
        sessions_.push_back(session);
        session->Start();
    }

    if (current_spawned_ < target_connections_) {
        ramp_up_timer_.expires_after(std::chrono::seconds(1));
        ramp_up_timer_.async_wait([this](boost::system::error_code ec) {
            if (!ec) ScheduleNextSpawn();
            });
    }
    else {
        std::cout << "\n✅ [StressTest] 모든 봇(" << target_connections_ << "개) 스폰 완료!\n\n";
    }
}

void StressManager::PrintMetricsLoop() {
    metrics_timer_.expires_after(std::chrono::seconds(1));
    metrics_timer_.async_wait([this](boost::system::error_code ec) {
        if (!ec) {
            int current_recv = total_packets_recv_.load();
            int tps = current_recv - last_recv_count_; // 1초간 수신한 패킷 수 (초당 처리량)
            last_recv_count_ = current_recv;

            std::cout << "[Metrics] 🌐 인게임 접속 유지: " << active_connections_.load() << "명 | "
                << "🚀 TPS(수신량): " << tps << " pkt/sec | "
                << "📤 누적 송신: " << total_packets_sent_.load() << "\r"; // \r로 줄바꿈 없이 덮어쓰기

            PrintMetricsLoop();
        }
        });
}