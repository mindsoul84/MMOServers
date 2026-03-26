#pragma once
#include <string>
#include <random>
#include <sstream>
#include <iomanip>

// ==========================================
// 보안 토큰 생성 유틸리티 추가
//
// 기존 문제: "WORLD_1_TOKEN_myid" 같은 예측 가능한 토큰
// 수정: 암호학적으로 안전한 랜덤 hex 문자열 생성
// ==========================================

class TokenUtils {
public:
    // 지정된 바이트 수의 랜덤 hex 문자열 생성 (기본 32바이트 = 64자)
    static std::string GenerateSessionToken(size_t byte_length = 32) {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;

        std::ostringstream oss;
        oss << std::hex << std::setfill('0');

        size_t remaining = byte_length;
        while (remaining > 0) {
            uint64_t val = dist(gen);
            size_t chunk = (remaining >= 8) ? 8 : remaining;
            oss << std::setw(static_cast<int>(chunk * 2)) << (val >> ((8 - chunk) * 8));
            remaining -= chunk;
        }
        return oss.str();
    }
};
