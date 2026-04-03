#pragma once

// ==========================================
//   게임 서버 상수 정의
// 하드코딩된 매직 넘버들을 한 곳에서 관리
// ==========================================

namespace GameConstants {

    // ---------------------------------------------------------
    // 맵/존 설정
    // ---------------------------------------------------------
    namespace Map {
        constexpr float WIDTH = 1000.0f;            // 맵 가로 크기
        constexpr float HEIGHT = 1000.0f;           // 맵 세로 크기
        constexpr int SECTOR_SIZE = 50;             // 섹터 크기 (AOI 단위)
    }

    // ---------------------------------------------------------
    // 플레이어 설정
    // ---------------------------------------------------------
    namespace Player {
        constexpr int DEFAULT_HP = 100;             // 기본 체력
        constexpr int DEFAULT_ATK = 30;             // 기본 공격력
        constexpr int DEFAULT_DEF = 5;              // 기본 방어력
        constexpr float SPAWN_X = 0.0f;             // 부활 X 좌표
        constexpr float SPAWN_Y = 0.0f;             // 부활 Y 좌표
    }

    // ---------------------------------------------------------
    // 몬스터 설정
    // ---------------------------------------------------------
    namespace Monster {
        constexpr int DEFAULT_HP = 100;             // 기본 체력
        constexpr int DEFAULT_ATK = 15;             // 기본 공격력
        constexpr int DEFAULT_DEF = 10;             // 기본 방어력
        constexpr float MOVE_SPEED = 2.0f;          // 이동 속도 (units/sec)
        constexpr float ATTACK_RANGE = 0.5f;        // 공격 사거리
        constexpr float ATTACK_COOLDOWN = 2.0f;     // 공격 쿨타임 (초)
        constexpr float CHASE_RANGE = 3.0f;         // 추적 포기 거리
        constexpr float AGGRO_RANGE = 1.0f;         // 어그로 범위
    }

    // ---------------------------------------------------------
    // 전투 설정
    // ---------------------------------------------------------
    namespace Combat {
        constexpr int MIN_DAMAGE = 1;               // 최소 데미지
        constexpr float PLAYER_ATTACK_RANGE = 1.5f; // 플레이어 공격 사거리
    }

    // ---------------------------------------------------------
    // 네트워크 설정
    // ---------------------------------------------------------
    namespace Network {
        constexpr int MAX_AOI_BROADCAST = 20;           // AOI 브로드캐스트 최대 인원
        constexpr float MONSTER_SYNC_INTERVAL = 2.0f;   // 몬스터 위치 동기화 주기 (초)
        constexpr int SEND_QUEUE_MAX_SIZE = 100000;     // 전송 큐 최대 크기
        constexpr int MAX_RETRIES = 3;                  // 네트워크 재시도 횟수
    }

    // ---------------------------------------------------------
    // AI 설정
    // ---------------------------------------------------------
    namespace AI {
        constexpr int TICK_INTERVAL_MS = 100;       // AI 업데이트 주기 (밀리초)
        constexpr float POSITION_EPSILON = 0.05f;   // 위치 변경 감지 임계값
        constexpr float WAYPOINT_EPSILON = 0.1f;    // 웨이포인트 도달 임계값
    }

} // namespace GameConstants
