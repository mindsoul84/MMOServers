#pragma once
#include <string>

class CryptoUtils {
public:
    // 무작위 Salt 문자열 생성 (기본 16자리)
    static std::string GenerateSalt(size_t length = 16);

    // 평문 비밀번호와 Salt를 결합하여 SHA-256 해시값(Hex 문자열) 반환
    static std::string HashPasswordSHA256(const std::string& password, const std::string& salt);
};