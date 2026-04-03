#pragma once

// ==========================================
//   보안 관련 상수 정의
// 서버 전체에서 공통으로 사용되는 보안 설정값을 한 곳에서 관리
// ==========================================

namespace SecurityConstants {

    // ---------------------------------------------------------
    // 패킷 유효성 검증
    // ---------------------------------------------------------
    namespace Packet {
        constexpr int MAX_PARSE_VIOLATIONS = 5;         // ParseFromArray 연속 실패 허용 횟수
        constexpr int MAX_RATE_VIOLATIONS = 5;          // Rate Limit 연속 초과 허용 횟수
        constexpr int MAX_PACKETS_PER_SECOND = 200;     // 초당 최대 허용 패킷 수
        constexpr int RATE_WINDOW_SECONDS = 1;          // Rate Limiting 측정 윈도우 (초)
    }

    // ---------------------------------------------------------
    // 세션 토큰 설정
    // ---------------------------------------------------------
    namespace Token {
        constexpr int TOKEN_BYTE_LENGTH = 32;           // 토큰 바이트 길이 (64자 hex)
        constexpr int64_t TOKEN_LIFETIME_MS = 300000;   // 토큰 유효 시간 (5분, 밀리초)
    }

    // ---------------------------------------------------------
    // 암호화 설정
    // ---------------------------------------------------------
    namespace Crypto {
        constexpr int AES_KEY_SIZE = 16;                // AES-128 키 크기 (바이트)
        constexpr int AES_BLOCK_SIZE = 16;              // AES 블록 크기 (바이트)
        constexpr int IV_SIZE = 16;                     // 초기화 벡터 크기 (바이트)
        constexpr int SEQUENCE_NUM_SIZE = 4;            // 시퀀스 번호 크기 (바이트)
        constexpr uint32_t SEQUENCE_WINDOW = 100;       // 시퀀스 검증 윈도우 크기

        //   클라이언트-서버 간 사전 공유 패스프레이즈 (Pre-Shared Key)
        //
        // PacketCrypto::InitializeWithPassphrase()에 전달되어
        // SHA-256(passphrase)의 앞 16바이트가 AES-128 키로 사용됩니다.
        //
        // [주의] 프로덕션 환경에서는 이 방식 대신 TLS 또는
        // Diffie-Hellman 키 교환을 통해 세션별 고유 키를 협상해야 합니다.
        // 사전 공유 키는 개발/테스트 환경에서의 암호화 파이프라인 검증 목적입니다.
        inline constexpr const char* SHARED_PASSPHRASE = "MMOSERVER_AES128_SHARED_KEY_2025";
    }

    // ---------------------------------------------------------
    // 입력값 검증
    // ---------------------------------------------------------
    namespace Input {
        constexpr size_t MAX_ACCOUNT_ID_LENGTH = 50;    // 계정 ID 최대 길이
        constexpr size_t MAX_PASSWORD_LENGTH = 100;     // 비밀번호 최대 길이
        constexpr int SALT_LENGTH = 16;                 // 솔트 길이
    }

} // namespace SecurityConstants
