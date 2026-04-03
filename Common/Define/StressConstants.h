#pragma once

// ==========================================
//   부하 테스트 봇 상수 정의
// StressTestTool에 하드코딩된 매직 넘버를 한 곳에서 관리
// ==========================================

namespace StressConstants {

    // ---------------------------------------------------------
    // 봇 인증 설정
    // ---------------------------------------------------------
    namespace Auth {
        inline constexpr const char* BOT_PASSWORD = "1234";     // 봇 공통 비밀번호
        constexpr int DEFAULT_WORLD_ID = 1;                     // 기본 접속 월드 ID
    }

    // ---------------------------------------------------------
    // 봇 AI 행동 설정
    // ---------------------------------------------------------
    namespace BotAI {
        constexpr int ACTION_MOVE_PERCENT = 70;         // 이동 확률 (%, 나머지는 공격)
        constexpr int MIN_ACTION_DELAY_MS = 2000;       // 최소 행동 대기 시간 (밀리초)
        constexpr int MAX_ACTION_DELAY_MS = 4000;       // 최대 행동 대기 시간 (밀리초)
        constexpr float MOVE_RANGE = 5.0f;              // 이동 범위 (-5 ~ +5)
        constexpr float SPAWN_RANGE = 1000.0f;          // 초기 스폰 좌표 범위
    }

    // ---------------------------------------------------------
    // 봇 재접속 설정
    // ---------------------------------------------------------
    namespace Reconnect {
        constexpr int RECONNECT_CHECK_INTERVAL_SEC = 1;     // 재접속 루프 주기 (초)
        constexpr int METRICS_PRINT_INTERVAL_SEC = 1;       // 메트릭 출력 주기 (초)
    }

} // namespace StressConstants
