#include "CryptoUtils.h"
#include <windows.h>
#include <wincrypt.h>
#include <random>
#include <sstream>
#include <iomanip>

std::string CryptoUtils::GenerateSalt(size_t length) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string salt;
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

    for (size_t i = 0; i < length; ++i) {
        salt += charset[dist(rng)];
    }
    return salt;
}

std::string CryptoUtils::HashPasswordSHA256(const std::string& password, const std::string& salt) {
    std::string data = password + salt; // 비밀번호에 Salt를 이어 붙임
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string hashStr = "";

    // Windows CryptoAPI를 이용한 SHA-256 해싱
    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
            if (CryptHashData(hHash, (const BYTE*)data.data(), (DWORD)data.size(), 0)) {
                DWORD hashLen = 0;
                DWORD count = sizeof(DWORD);
                if (CryptGetHashParam(hHash, HP_HASHSIZE, (BYTE*)&hashLen, &count, 0)) {
                    std::vector<BYTE> buffer(hashLen);
                    if (CryptGetHashParam(hHash, HP_HASHVAL, buffer.data(), &hashLen, 0)) {
                        std::ostringstream oss;
                        for (BYTE b : buffer) {
                            oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
                        }
                        hashStr = oss.str(); // 64자리 Hex 문자열 생성
                    }
                }
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }
    return hashStr;
}