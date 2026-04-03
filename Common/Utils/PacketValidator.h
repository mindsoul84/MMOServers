#pragma once
#include <cstdint>
#include <string>
#include <iostream>
#include <functional>

// ==========================================
// [수정] 패킷 유효성 검증 + 위반 추적 유틸리티
//
// [문제점]
// 1. ParseFromArray 실패 시 로그만 출력하고 세션을 유지
//    -> 악의적 클라이언트가 깨진 페이로드를 지속 전송해도 아무 조치 없음
// 2. payload_buf_.resize(payload_size)로 매번 동적 할당
//    -> 공격자가 header.size를 MAX_PACKET_SIZE 이하로 반복 전송 시 할당 부하
// 3. 패킷 검증 로직이 각 세션 클래스에 산발적으로 분포
//    -> 일관성 없는 검증, 누락 위험
//
// [수정 사항]
// 1. 위반(violation) 카운트 추적 및 자동 차단 판정
//    -> ParseFromArray 실패, 잘못된 헤더 크기, 미등록 패킷 ID를
//       모두 위반으로 카운팅
//    -> 연속 위반 시 ShouldDisconnect()가 true를 반환하여 세션 종료 유도
//    -> 정상 패킷 수신 시 카운터 리셋으로 일시적 오류 허용
// 2. 고정 크기 수신 버퍼 제공 (동적 할당 제거)
//    -> MAX_PAYLOAD_SIZE 크기의 버퍼를 멤버로 보유
//    -> payload_buf_.resize() 호출 불필요
// 3. 단일 클래스로 검증 로직 통합
//    -> 모든 세션 클래스가 동일한 검증 체계 사용
// ==========================================

class PacketValidator {
public:
    // 패킷 헤더 최소 크기 (PacketHeader = 4 bytes)
    static constexpr uint16_t HEADER_SIZE = 4;

    // 페이로드 최대 크기 (PacketHeader 제외)
    static constexpr uint16_t MAX_PAYLOAD_SIZE = 4096 - HEADER_SIZE;

    // 위반 유형
    enum class ViolationType {
        INVALID_HEADER_SIZE,        // 헤더 크기가 비정상 (< HEADER_SIZE 또는 > MAX)
        UNREGISTERED_PACKET_ID,     // 등록되지 않은 패킷 ID
        PARSE_FAILURE,              // Protobuf ParseFromArray 실패
        PAYLOAD_TOO_LARGE,          // 페이로드 크기 초과
        SEQUENCE_VIOLATION          // 시퀀스 번호 위반 (암호화 계층에서 사용)
    };

private:
    // 위반 추적 상태
    int violation_count_ = 0;
    int total_violations_ = 0;
    int max_consecutive_violations_;
    int max_total_violations_;

    // 고정 크기 수신 버퍼 (동적 할당 제거)
    char recv_buffer_[MAX_PAYLOAD_SIZE];

    // 식별 정보 (로그용)
    std::string session_tag_;

public:
    // max_consecutive: 연속 위반 허용 횟수 (기본 5회)
    // max_total: 누적 위반 허용 횟수 (기본 50회, 세션 수명 동안)
    PacketValidator(const std::string& tag = "Session",
                    int max_consecutive = 5,
                    int max_total = 50)
        : session_tag_(tag)
        , max_consecutive_violations_(max_consecutive)
        , max_total_violations_(max_total) {}

    // ---------------------------------------------------------
    // 패킷 헤더 크기 검증
    // 반환: true = 유효, false = 위반
    // ---------------------------------------------------------
    bool ValidateHeaderSize(uint16_t packet_size) {
        if (packet_size < HEADER_SIZE || packet_size > (HEADER_SIZE + MAX_PAYLOAD_SIZE)) {
            RecordViolation(ViolationType::INVALID_HEADER_SIZE,
                "잘못된 패킷 크기: " + std::to_string(packet_size));
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------
    // 페이로드 크기 검증
    // 반환: true = 유효, false = 위반
    // ---------------------------------------------------------
    bool ValidatePayloadSize(uint16_t payload_size) {
        if (payload_size > MAX_PAYLOAD_SIZE) {
            RecordViolation(ViolationType::PAYLOAD_TOO_LARGE,
                "페이로드 크기 초과: " + std::to_string(payload_size));
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------
    // ParseFromArray 결과 검증
    // parse_success: ParseFromArray()의 반환값
    // 반환: true = 성공, false = 실패 (위반 기록됨)
    // ---------------------------------------------------------
    bool ValidateParse(bool parse_success, const char* handler_name, uint16_t payload_size) {
        if (!parse_success) {
            RecordViolation(ViolationType::PARSE_FAILURE,
                std::string(handler_name) + " ParseFromArray 실패 (payloadSize=" +
                std::to_string(payload_size) + ")");
            return false;
        }
        // 정상 파싱 시 연속 위반 카운터 리셋
        ResetConsecutive();
        return true;
    }

    // ---------------------------------------------------------
    // 위반 기록
    // ---------------------------------------------------------
    void RecordViolation(ViolationType type, const std::string& detail = "") {
        violation_count_++;
        total_violations_++;

        const char* type_str = "";
        switch (type) {
            case ViolationType::INVALID_HEADER_SIZE:    type_str = "INVALID_HEADER"; break;
            case ViolationType::UNREGISTERED_PACKET_ID: type_str = "UNREGISTERED_ID"; break;
            case ViolationType::PARSE_FAILURE:          type_str = "PARSE_FAILURE"; break;
            case ViolationType::PAYLOAD_TOO_LARGE:      type_str = "PAYLOAD_TOO_LARGE"; break;
            case ViolationType::SEQUENCE_VIOLATION:     type_str = "SEQUENCE_VIOLATION"; break;
        }

        std::cerr << "[PacketValidator] [" << session_tag_ << "] "
                  << type_str << " (연속:" << violation_count_
                  << "/" << max_consecutive_violations_
                  << ", 누적:" << total_violations_
                  << "/" << max_total_violations_ << ")";
        if (!detail.empty()) {
            std::cerr << " - " << detail;
        }
        std::cerr << "\n";
    }

    // ---------------------------------------------------------
    // 연속 위반 카운터 리셋 (정상 패킷 수신 시)
    // ---------------------------------------------------------
    void ResetConsecutive() {
        violation_count_ = 0;
    }

    // ---------------------------------------------------------
    // 연결 종료 판정
    // 연속 위반 또는 누적 위반이 임계치를 초과하면 true 반환
    // ---------------------------------------------------------
    bool ShouldDisconnect() const {
        return (violation_count_ >= max_consecutive_violations_) ||
               (total_violations_ >= max_total_violations_);
    }

    // ---------------------------------------------------------
    // 고정 크기 수신 버퍼 접근
    // payload_buf_.resize() 대신 이 버퍼를 사용하여 동적 할당 제거
    // ---------------------------------------------------------
    char* GetRecvBuffer() { return recv_buffer_; }
    const char* GetRecvBuffer() const { return recv_buffer_; }
    static constexpr uint16_t GetRecvBufferSize() { return MAX_PAYLOAD_SIZE; }

    // 통계 접근자 (디버깅/로깅용)
    int GetConsecutiveViolations() const { return violation_count_; }
    int GetTotalViolations() const { return total_violations_; }

    void SetSessionTag(const std::string& tag) { session_tag_ = tag; }
};
