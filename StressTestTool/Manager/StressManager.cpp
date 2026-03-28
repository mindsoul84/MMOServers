#include "StressManager.h"
#include "../Session/StressSession.h"
#include <iostream>

StressManager::StressManager(boost::asio::io_context& io, int target_conn, int spawn_rate)
    : io_context_(io), ramp_up_timer_(io), reconnect_timer_(io), metrics_timer_(io),
    target_connections_(target_conn), spawn_rate_per_sec_(spawn_rate) {
}

void StressManager::StartStressTest() {
    std::cout << "🚀 [StressTest] 부하 테스트 시작! 목표 인원: " << target_connections_ << "명\n";
    ScheduleNextSpawn();
    StartReconnectLoop();
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

// ==========================================
// [수정] OnSessionDisconnected에서 타이머 조작 제거
//
// 변경 전:
//   ramp_up_timer_.expires_after(500ms) 호출
//   -> 여러 봇이 동시에 끊어질 때 이전 타이머를 취소하여
//      재스폰 요청이 누락되는 문제 발생
//
// 변경 후:
//   세션 정리(sessions_.erase, current_spawned_ 감소)만 수행
//   재스폰은 독립 유지보수 루프(StartReconnectLoop)가 1초 주기로 처리
//   -> 타이머 취소 문제가 근본적으로 발생하지 않음
// ==========================================
void StressManager::OnSessionDisconnected(std::shared_ptr<StressSession> session, bool was_in_game) {
    if (was_in_game) {
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(manager_mtx_);
    sessions_.erase(session);
    current_spawned_--;
}

// ==========================================
// [추가] 독립 재접속 유지보수 루프
//
// 1초 주기로 current_spawned_와 target_connections_를 비교하여
// 부족분만큼 새 봇을 생성합니다.
//
// ramp_up_timer_(초기 스폰)와 완전히 독립적으로 동작하므로
// 타이머 취소(expires_after)로 인한 재스폰 누락이 발생하지 않습니다.
//
// ScheduleNextSpawn의 초기 스폰이 완료된 이후에도 계속 동작하여
// 게임 도중 끊어진 봇을 지속적으로 보충합니다.
// ==========================================
void StressManager::StartReconnectLoop() {
    reconnect_timer_.expires_after(std::chrono::seconds(1));
    reconnect_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;

        {
            std::lock_guard<std::mutex> lock(manager_mtx_);
            int deficit = target_connections_ - current_spawned_;
            if (deficit > 0) {
                int spawn_count = std::min(spawn_rate_per_sec_, deficit);
                for (int i = 0; i < spawn_count; ++i) {
                    std::string bot_id = "BOT_STRESS_" + std::to_string(total_spawn_count_++);
                    auto session = std::make_shared<StressSession>(io_context_, this, bot_id);
                    sessions_.insert(session);
                    current_spawned_++;
                    session->Start();
                }
            }
        }

        StartReconnectLoop();
    });
}

void StressManager::PrintMetricsLoop() {
    metrics_timer_.expires_after(std::chrono::seconds(1));
    metrics_timer_.async_wait([this](boost::system::error_code ec) {
        if (!ec) {
            uint64_t current_recv = total_packets_recv_.load();
            uint64_t tps = current_recv - last_recv_count_;
            last_recv_count_ = current_recv;

            std::cout << "[Metrics] 🌐 활성 봇: " << active_connections_.load() << "명 | "
                << "📥 패킷 수신: " << tps << " pkt/sec | "
                << "📥 누적 송신: " << total_packets_sent_.load() << "      \r";

            PrintMetricsLoop();
        }
    });
}
