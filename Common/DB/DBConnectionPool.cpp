#include "DBConnectionPool.h"
#include "../Utils/Logger.h"

bool DBConnectionPool::Initialize(size_t pool_size) {
    if (initialized_) return true;

    UTILITY::LockGuard lock(mutex_);
    pool_size_ = pool_size;

    for (size_t i = 0; i < pool_size; ++i) {
        auto conn = std::make_unique<DBManager>();
        if (conn->Connect()) {
            available_.push(std::move(conn));
            total_created_++;
            LOG_INFO("DBPool", "연결 " << total_created_ << "/" << pool_size << " 생성 성공");
        }
        else {
            LOG_ERROR("DBPool", "연결 " << (i + 1) << "/" << pool_size << " 생성 실패");
        }
    }

    initialized_ = true;
    LOG_INFO("DBPool", "DB 연결 풀 초기화 완료: " << total_created_
        << "/" << pool_size << "개 연결 활성화");
    return total_created_ > 0;
}

DBConnectionPool::ScopedConnection DBConnectionPool::Acquire(int timeout_ms) {
    UTILITY::UniqueLock lock(mutex_);

    if (!initialized_) {
        LOG_ERROR("DBPool", "풀이 초기화되지 않은 상태에서 Acquire 호출");
        return ScopedConnection();
    }

    // 사용 가능한 연결이 생길 때까지 대기 (타임아웃 적용)
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
        [this]() { return !available_.empty(); })) {
        LOG_WARN("DBPool", "연결 획득 타임아웃 (" << timeout_ms << "ms) - 모든 연결이 사용 중");
        return ScopedConnection();
    }

    auto conn = std::move(available_.front());
    available_.pop();
    return ScopedConnection(this, std::move(conn));
}

void DBConnectionPool::ReturnConnection(std::unique_ptr<DBManager> conn) {
    if (!conn) return;

    UTILITY::LockGuard lock(mutex_);
    available_.push(std::move(conn));
    cv_.notify_one();
}

size_t DBConnectionPool::GetTotalCreated() const {
    UTILITY::LockGuard lock(mutex_);
    return total_created_;
}

size_t DBConnectionPool::GetAvailableCount() const {
    UTILITY::LockGuard lock(mutex_);
    return available_.size();
}
