#include "StressManager.h"
#include "../Session/StressSession.h"
#include <iostream>

StressManager::StressManager(boost::asio::io_context& io, int target_conn, int spawn_rate)
    : io_context_(io), ramp_up_timer_(io), reconnect_timer_(io), metrics_timer_(io),
    target_connections_(target_conn), spawn_rate_per_sec_(spawn_rate) {
}

// ==========================================
// 봇 ID 발급 함수 (재사용 큐 우선)
//
// 1. reusable_ids_ 큐에 반환된 ID가 있으면 그것을 재사용
//    -> 같은 ID로 재로그인하면 DB에서 기존 계정을 찾아 SUCCESS 반환
//    -> Redis에서도 같은 키("player:BOT_STRESS_N")에 덮어쓰기
// 2. 큐가 비어있으면 total_spawn_count_++로 새 ID를 생성
//    -> 최초 스폰 시에만 새 ID가 생성됨
//
// 주의: manager_mtx_ 보유 상태에서 호출해야 함 (reusable_ids_ 접근)
// ==========================================
std::string StressManager::AcquireBotId() {
    if (!reusable_ids_.empty()) {
        std::string reused_id = reusable_ids_.front();
        reusable_ids_.pop();
        return reused_id;
    }
    return "BOT_STRESS_" + std::to_string(total_spawn_count_++);
}

void StressManager::StartStressTest() {
    std::cout << "🚀 [StressTest] 부하 테스트 시작! 목표 인원: " << target_connections_ << "명\n";
    ScheduleNextSpawn();
    StartReconnectLoop();
    PrintMetricsLoop();
}

void StressManager::ScheduleNextSpawn() {
    UTILITY::LockGuard lock(manager_mtx_);

    int spawn_count = std::min(spawn_rate_per_sec_, target_connections_ - current_spawned_);

    for (int i = 0; i < spawn_count; ++i) {
        // 직접 ID 생성 대신 AcquireBotId() 호출로 재사용 우선
        std::string bot_id = AcquireBotId();
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
// OnSessionDisconnected - 끊어진 봇의 ID를 재사용 큐에 반환
//
// 변경 전: 세션 정리만 수행, 봇 ID는 버려짐
//   -> 재스폰 시 total_spawn_count_++로 항상 새 ID 발급
//   -> DB/Redis에 무한히 새 엔트리 누적
//
// 변경 후: 끊어진 봇의 account_id를 reusable_ids_ 큐에 push
//   -> 재스폰 시 AcquireBotId()가 이 큐에서 ID를 꺼내 재사용
//   -> 같은 ID로 재로그인하므로 DB INSERT 없음, Redis 덮어쓰기
// ==========================================
void StressManager::OnSessionDisconnected(std::shared_ptr<StressSession> session, bool was_in_game) {
    if (was_in_game) {
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    }

    UTILITY::LockGuard lock(manager_mtx_);

    // 끊어진 봇의 ID를 재사용 큐에 반환
    const std::string& disconnected_id = session->GetAccountId();
    if (!disconnected_id.empty()) {
        reusable_ids_.push(disconnected_id);
    }

    sessions_.erase(session);
    current_spawned_--;
}

// ==========================================
// 독립 재접속 유지보수 루프
//
// 1초 주기로 current_spawned_와 target_connections_를 비교하여
// 부족분만큼 새 봇을 생성합니다.
//
// AcquireBotId()를 사용하여 재사용 큐에서 ID를 우선 꺼냄
// ==========================================
void StressManager::StartReconnectLoop() {
    reconnect_timer_.expires_after(std::chrono::seconds(1));
    reconnect_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;

        {
            UTILITY::LockGuard lock(manager_mtx_);
            int deficit = target_connections_ - current_spawned_;
            if (deficit > 0) {
                int spawn_count = std::min(spawn_rate_per_sec_, deficit);
                for (int i = 0; i < spawn_count; ++i) {
                    // 직접 ID 생성 대신 AcquireBotId() 호출로 재사용 우선
                    std::string bot_id = AcquireBotId();
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
