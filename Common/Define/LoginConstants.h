#pragma once

// ==========================================
//   로그인 서버 상수 정의
// LoginServer에 하드코딩된 매직 넘버를 한 곳에서 관리
// ==========================================

namespace LoginConstants {

    // ---------------------------------------------------------
    // Heartbeat 설정
    // ---------------------------------------------------------
    namespace Heartbeat {
        constexpr int CHECK_INTERVAL_SECONDS = 15;  // 하트비트 체크 주기 (초)
        constexpr int TIMEOUT_SECONDS = 30;         // 타임아웃 임계값 (초)
    }

    // ---------------------------------------------------------
    // 세션 보안 설정
    // ---------------------------------------------------------
    namespace Security {
        constexpr int MAX_PARSE_VIOLATIONS = 5;     // ParseFromArray 연속 실패 허용 횟수
        constexpr int MAX_RATE_VIOLATIONS = 5;      // Rate Limit 연속 초과 허용 횟수
    }

} // namespace LoginConstants
