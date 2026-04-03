#pragma once

// ==========================================
//   DB 연결 풀 (Connection Pool)
//
// [변경 전 문제]
//   thread_local std::unique_ptr<DBManager> t_dbManager 방식으로
//   DB 스레드마다 고정된 1개의 ODBC 연결을 보유
//   -> DB 스레드 수(db_thread_count)와 동시 연결 수가 1:1로 묶임
//   -> 부하가 높아지면 스레드 수를 늘려야 하지만, 각 스레드가
//      독립적으로 연결을 관리하므로 연결 재사용이 불가능
//   -> 연결 헬스체크나 재연결 정책이 없음
//
// [변경 후]
//   DBConnectionPool이 N개의 ODBC 연결을 중앙에서 관리
//   -> 스레드 수와 연결 수를 독립적으로 설정 가능
//   -> ScopedConnection RAII 래퍼로 자동 반납 (누수 방지)
//   -> 연결 부족 시 타임아웃 기반 대기 (condition_variable)
//   -> 향후 연결 헬스체크, 동적 확장 등으로 확장 가능
//
// [사용 예]
//   DBConnectionPool pool;
//   pool.Initialize(4);  // 4개의 ODBC 연결 사전 생성
//
//   // DB 작업이 필요할 때:
//   {
//       auto conn = pool.Acquire(5000);  // 최대 5초 대기
//       if (conn) {
//           conn->ProcessLogin(id, pw, input_type, &uid);
//       }  // <-- 스코프 종료 시 자동으로 풀에 반납
//   }
// ==========================================

#include "DBManager.h"
#include "../Utils/Lock.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <string>

class DBConnectionPool {
public:
    // ==========================================
    // RAII 연결 래퍼 (Scoped Connection)
    //
    // 스코프를 벗어나면 자동으로 풀에 연결을 반납합니다.
    // 명시적 Release() 호출이 필요 없어 누수를 방지합니다.
    // ==========================================
    class ScopedConnection {
    public:
        ScopedConnection() : pool_(nullptr) {}

        ScopedConnection(DBConnectionPool* pool, std::unique_ptr<DBManager> conn)
            : pool_(pool), conn_(std::move(conn)) {}

        ~ScopedConnection() {
            if (conn_ && pool_) {
                pool_->ReturnConnection(std::move(conn_));
            }
        }

        // 이동 전용 (복사 금지)
        ScopedConnection(ScopedConnection&& other) noexcept
            : pool_(other.pool_), conn_(std::move(other.conn_)) {
            other.pool_ = nullptr;
        }

        ScopedConnection& operator=(ScopedConnection&& other) noexcept {
            if (this != &other) {
                if (conn_ && pool_) pool_->ReturnConnection(std::move(conn_));
                pool_ = other.pool_;
                conn_ = std::move(other.conn_);
                other.pool_ = nullptr;
            }
            return *this;
        }

        ScopedConnection(const ScopedConnection&) = delete;
        ScopedConnection& operator=(const ScopedConnection&) = delete;

        DBManager* operator->() { return conn_.get(); }
        DBManager* get() { return conn_.get(); }
        explicit operator bool() const { return conn_ != nullptr; }

    private:
        DBConnectionPool* pool_;
        std::unique_ptr<DBManager> conn_;
    };

    DBConnectionPool() = default;
    ~DBConnectionPool() = default;

    DBConnectionPool(const DBConnectionPool&) = delete;
    DBConnectionPool& operator=(const DBConnectionPool&) = delete;

    // ==========================================
    // 풀 초기화: pool_size개의 ODBC 연결을 사전 생성
    // 일부 연결 실패 시에도 1개 이상 성공하면 true 반환
    // ==========================================
    bool Initialize(size_t pool_size);

    // ==========================================
    // 연결 획득 (최대 timeout_ms 밀리초 대기)
    // 타임아웃 시 빈 ScopedConnection 반환 (operator bool() == false)
    // ==========================================
    ScopedConnection Acquire(int timeout_ms = 5000);

    // 통계 접근자
    size_t GetTotalCreated() const;
    size_t GetAvailableCount() const;
    bool IsInitialized() const { return initialized_; }

private:
    // ScopedConnection의 소멸자에서 호출
    void ReturnConnection(std::unique_ptr<DBManager> conn);

    std::queue<std::unique_ptr<DBManager>> available_;
    mutable UTILITY::Lock mutex_;
    std::condition_variable cv_;
    size_t pool_size_ = 0;
    size_t total_created_ = 0;
    bool initialized_ = false;
};
