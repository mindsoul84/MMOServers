#pragma once
#include <functional>
#include <array>
#include <memory>
#include <iostream>
#include <chrono>
#include <atomic>

#include "Define/SecurityConstants.h"

// ==========================================
// PacketDispatcher: 패킷 유효성 검증 및 Rate Limiting 추가
//
// [변경 전] 미등록 패킷 ID에 대해 로그만 출력, 세션별 처리량 제한 없음
//   -> 악의적 클라이언트가 초당 수만 개 패킷을 보내도 모두 처리
//   -> 서버 자원 고갈 공격(DoS)에 취약
//
// [변경 후]
//   1. HasHandler(): 패킷 ID 등록 여부 사전 검증 (ReadPayload에서 호출)
//   2. Dispatch()에서 핸들러 존재 확인 및 로그 출력
//   3. RateLimiter: 세션별 초당 패킷 제한 (Token Bucket 방식)
//      -> 기본 MAX_PACKETS_PER_SECOND = 200, 초과 시 패킷 드랍
// ==========================================

// ==========================================
// 세션별 Rate Limiter (Token Bucket 알고리즘)
//
// 각 세션(클라이언트)이 초당 보낼 수 있는 패킷 수를 제한합니다.
// 윈도우 기반 카운터 방식으로 구현하여 메모리와 연산 비용을 최소화합니다.
//
//   하드코딩된 매직 넘버를 SecurityConstants로 교체
// ==========================================
class PacketRateLimiter {
private:
    std::chrono::steady_clock::time_point window_start_;
    int packet_count_ = 0;

public:
    PacketRateLimiter()
        : window_start_(std::chrono::steady_clock::now()) {}

    // 패킷 처리 허용 여부 확인. false를 반환하면 드랍해야 함
    bool AllowPacket() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - window_start_).count();

        if (elapsed >= SecurityConstants::Packet::RATE_WINDOW_SECONDS) {
            // 윈도우 리셋
            window_start_ = now;
            packet_count_ = 1;
            return true;
        }

        packet_count_++;
        if (packet_count_ > SecurityConstants::Packet::MAX_PACKETS_PER_SECOND) {
            return false;  // Rate limit 초과 - 드랍
        }
        return true;
    }

    int GetCurrentCount() const { return packet_count_; }
};

// ==========================================
//   ParseFromArray 실패 추적기 (Parse Violation Tracker)
//
// ParseFromArray 실패 시 로그만 출력하고 세션을 유지하는 것은
// 악의적 클라이언트가 의도적으로 깨진 페이로드를 보내는 공격에 취약합니다.
// 연속 실패 횟수를 추적하여 임계값 초과 시 세션을 강제 종료합니다.
// ==========================================
class ParseViolationTracker {
private:
    int violation_count_ = 0;

public:
    // ParseFromArray 실패 시 호출. true를 반환하면 세션을 종료해야 함
    bool OnParseFailure() {
        violation_count_++;
        return violation_count_ >= SecurityConstants::Packet::MAX_PARSE_VIOLATIONS;
    }

    // 정상 패킷 수신 시 카운터 리셋
    void OnParseSuccess() {
        violation_count_ = 0;
    }

    int GetViolationCount() const { return violation_count_; }
};

template <typename SessionType>
class PacketDispatcher {
public:
    using HandlerFunc = std::function<void(std::shared_ptr<SessionType>&, char*, uint16_t)>;

private:
    std::unordered_map<uint16_t, HandlerFunc> handlers_;

public:
    // 패킷 핸들러 등록 (서버 초기화 시 메인 스레드에서만 호출되므로 락 불필요)
    void RegisterHandler(uint16_t pktId, HandlerFunc handler) {
        handlers_[pktId] = handler;
    }

    // 패킷 ID에 대한 핸들러 등록 여부 확인 (ReadHeader에서 사전 검증용)
    bool HasHandler(uint16_t pktId) const {
        return handlers_.find(pktId) != handlers_.end();
    }

    // 등록된 핸들러 수 반환 (디버깅용)
    size_t GetHandlerCount() const {
        return handlers_.size();
    }

    // 패킷 분배 (읽기 전용이므로 멀티스레드 환경에서도 안전함)
    void Dispatch(std::shared_ptr<SessionType>& session, uint16_t pktId, char* payload, uint16_t payloadSize) {
        auto it = handlers_.find(pktId);
        if (it != handlers_.end()) {
            if (it->second) {
                it->second(session, payload, payloadSize);
            }
        }
        else {
            std::cerr << "[PacketDispatcher] 등록되지 않은 패킷 ID 수신: " << pktId
                      << " (payloadSize=" << payloadSize << ") - 무시됨\n";
        }
    }
};
