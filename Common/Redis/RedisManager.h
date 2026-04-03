#pragma once
#include <string>
#include <mutex>
#include <memory>
#include <iostream>
#include "../Utils/Lock.h"
#pragma warning(push)
#pragma warning(disable: 4200) // 비표준 확장: 구조체/공용 구조체의 배열 크기가 0 (hiredis sds.h)
#include <hiredis/hiredis.h>
#pragma warning(pop)

// ==========================================
// Redis 연동 매니저
//
// config.json의 redis_info 설정에 따라 Redis 서버에 연결하고,
// 유저가 게임서버에 접속하면 account_id와 HP를 Redis에 저장합니다.
// 실시간으로 HP를 갱신하며, 접속 종료 시 삭제합니다.
//
// Redis Key 설계:
//   Key:    "player:{account_id}"   (HASH)
//   Fields: account_id (string), hp (int)
//
// [사용 예]
//   RedisManager::GetInstance().Connect("127.0.0.1", 6379);
//   RedisManager::GetInstance().SetPlayerOnline("myid", 100);
//   RedisManager::GetInstance().UpdatePlayerHp("myid", 80);
//   RedisManager::GetInstance().RemovePlayer("myid");
// ==========================================

class RedisManager {
private:
    redisContext* context_ = nullptr;
    UTILITY::Lock mutex_;  // hiredis의 redisContext는 스레드 안전하지 않으므로 락 필요
    bool connected_ = false;

    // DI(의존성 주입) 지원 (ConfigManager, GameContext와 동일 패턴)
    inline static RedisManager* s_test_instance_ = nullptr;

public:
    RedisManager() = default;
    ~RedisManager() { Disconnect(); }

    RedisManager(const RedisManager&) = delete;
    RedisManager& operator=(const RedisManager&) = delete;

    // 싱글톤 + 테스트 인스턴스 주입 지원
    static RedisManager& GetInstance() {
        if (s_test_instance_) return *s_test_instance_;
        static RedisManager instance;
        return instance;
    }

    static void SetTestInstance(RedisManager* instance) noexcept {
        s_test_instance_ = instance;
    }

    // ---------------------------------------------------------
    // Redis 서버 연결
    // ---------------------------------------------------------
    bool Connect(const std::string& host = "127.0.0.1", int port = 6379, int timeout_ms = 3000) {
        UTILITY::LockGuard lock(mutex_);

        if (connected_ && context_) return true;

        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        context_ = redisConnectWithTimeout(host.c_str(), port, tv);

        if (!context_) {
            std::cerr << "[RedisManager] Redis 연결 실패: 메모리 할당 오류\n";
            return false;
        }

        if (context_->err) {
            std::cerr << "[RedisManager] Redis 연결 실패: " << context_->errstr << "\n";
            redisFree(context_);
            context_ = nullptr;
            return false;
        }

        connected_ = true;
        std::cout << "[RedisManager] Redis 서버 연결 성공! (" << host << ":" << port << ")\n";
        return true;
    }

    // ---------------------------------------------------------
    // Redis 연결 해제
    // ---------------------------------------------------------
    void Disconnect() {
        UTILITY::LockGuard lock(mutex_);
        if (context_) {
            redisFree(context_);
            context_ = nullptr;
            connected_ = false;
            std::cout << "[RedisManager] Redis 연결 해제됨.\n";
        }
    }

    bool IsConnected() const { return connected_; }

    // ---------------------------------------------------------
    // 유저 접속 시: account_id와 HP를 Redis HASH로 저장
    //   Key: "player:{account_id}"
    //   Fields: account_id, hp
    // ---------------------------------------------------------
    bool SetPlayerOnline(const std::string& account_id, int hp) {
        UTILITY::LockGuard lock(mutex_);
        if (!connected_ || !context_) return false;

        std::string key = "player:" + account_id;

        // HSET를 필드별로 분리 호출 (Redis 4.0 미만 호환)
        redisReply* reply1 = static_cast<redisReply*>(
            redisCommand(context_, "HSET %s account_id %s", key.c_str(), account_id.c_str())
        );
        if (!reply1) { HandleDisconnection(); return false; }
        if (reply1->type == REDIS_REPLY_ERROR) {
            std::cerr << "[RedisManager] SetPlayerOnline(account_id) 실패: " << reply1->str << "\n";
            freeReplyObject(reply1);
            return false;
        }
        freeReplyObject(reply1);

        redisReply* reply2 = static_cast<redisReply*>(
            redisCommand(context_, "HSET %s hp %d", key.c_str(), hp)
        );
        if (!reply2) { HandleDisconnection(); return false; }
        if (reply2->type == REDIS_REPLY_ERROR) {
            std::cerr << "[RedisManager] SetPlayerOnline(hp) 실패: " << reply2->str << "\n";
            freeReplyObject(reply2);
            return false;
        }
        freeReplyObject(reply2);

        return true;
    }

    // ---------------------------------------------------------
    // HP 실시간 갱신
    // ---------------------------------------------------------
    bool UpdatePlayerHp(const std::string& account_id, int new_hp) {
        UTILITY::LockGuard lock(mutex_);
        if (!connected_ || !context_) return false;

        std::string key = "player:" + account_id;

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(context_, "HSET %s hp %d", key.c_str(), new_hp)
        );

        if (!reply) {
            HandleDisconnection();
            return false;
        }

        bool success = (reply->type != REDIS_REPLY_ERROR);
        freeReplyObject(reply);
        return success;
    }

    // ---------------------------------------------------------
    // 유저 접속 종료 시: Redis에서 해당 유저 데이터 삭제
    // ---------------------------------------------------------
    bool RemovePlayer(const std::string& account_id) {
        UTILITY::LockGuard lock(mutex_);
        if (!connected_ || !context_) return false;

        std::string key = "player:" + account_id;

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(context_, "DEL %s", key.c_str())
        );

        if (!reply) {
            HandleDisconnection();
            return false;
        }

        bool success = (reply->type != REDIS_REPLY_ERROR);
        freeReplyObject(reply);
        return success;
    }

    // ---------------------------------------------------------
    // [디버깅용] 특정 유저의 Redis 저장 HP 조회
    // ---------------------------------------------------------
    int GetPlayerHp(const std::string& account_id) {
        UTILITY::LockGuard lock(mutex_);
        if (!connected_ || !context_) return -1;

        std::string key = "player:" + account_id;

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(context_, "HGET %s hp", key.c_str())
        );

        if (!reply) {
            HandleDisconnection();
            return -1;
        }

        int hp = -1;
        if (reply->type == REDIS_REPLY_STRING && reply->str) {
            hp = std::atoi(reply->str);
        }
        freeReplyObject(reply);
        return hp;
    }

private:
    // 연결이 끊어진 경우 상태 초기화
    void HandleDisconnection() {
        std::cerr << "[RedisManager] Redis 연결이 끊어졌습니다.\n";
        if (context_) {
            redisFree(context_);
            context_ = nullptr;
        }
        connected_ = false;
    }
};
