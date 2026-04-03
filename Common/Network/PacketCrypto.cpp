#include "PacketCrypto.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <iostream>

#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// ==========================================
//   AES-128-CBC 암호화 구현 (Windows BCrypt API)
//
// BCrypt API는 Windows Vista 이후 제공되는 차세대 암호화 API로,
// 레거시 CryptoAPI(CryptAcquireContext 등)보다 성능과 보안이 우수합니다.
// FIPS 140-2 인증을 받은 OS 수준 암호 모듈을 사용합니다.
// ==========================================

PacketCrypto::~PacketCrypto() {
    if (hKey_) {
        BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(hKey_));
        hKey_ = nullptr;
    }
    if (hAlgorithm_) {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(hAlgorithm_), 0);
        hAlgorithm_ = nullptr;
    }
}

bool PacketCrypto::Initialize(const unsigned char* key, size_t key_len) {
    if (key_len != CryptoConstants::AES_KEY_SIZE) {
        std::cerr << "[PacketCrypto] 키 크기 오류: " << key_len << "바이트 (16바이트 필요)\n";
        return false;
    }

    std::memcpy(key_, key, CryptoConstants::AES_KEY_SIZE);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!NT_SUCCESS(status)) {
        std::cerr << "[PacketCrypto] BCryptOpenAlgorithmProvider 실패: 0x" << std::hex << status << "\n";
        return false;
    }
    hAlgorithm_ = hAlg;

    // CBC 모드 설정
    status = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
        (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (!NT_SUCCESS(status)) {
        std::cerr << "[PacketCrypto] CBC 모드 설정 실패: 0x" << std::hex << status << "\n";
        return false;
    }

    // 키 객체 생성
    BCRYPT_KEY_HANDLE hKey = nullptr;
    status = BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
        (PUCHAR)key_, CryptoConstants::AES_KEY_SIZE, 0);
    if (!NT_SUCCESS(status)) {
        std::cerr << "[PacketCrypto] BCryptGenerateSymmetricKey 실패: 0x" << std::hex << status << "\n";
        return false;
    }
    hKey_ = hKey;

    initialized_ = true;
    return true;
}

bool PacketCrypto::InitializeWithPassphrase(const std::string& passphrase) {
    // 패스프레이즈를 SHA-256 해싱 후 앞 16바이트를 AES 키로 사용
    BCRYPT_ALG_HANDLE hHashAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    unsigned char hash_output[32] = { 0 }; // SHA-256 = 32바이트
    ULONG hash_len = 32;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&hHashAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!NT_SUCCESS(status)) return false;

    status = BCryptCreateHash(hHashAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (!NT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hHashAlg, 0);
        return false;
    }

    status = BCryptHashData(hHash, (PUCHAR)passphrase.c_str(), (ULONG)passphrase.size(), 0);
    if (!NT_SUCCESS(status)) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hHashAlg, 0);
        return false;
    }

    status = BCryptFinishHash(hHash, hash_output, hash_len, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hHashAlg, 0);

    if (!NT_SUCCESS(status)) return false;

    // SHA-256 결과의 앞 16바이트를 AES-128 키로 사용
    return Initialize(hash_output, CryptoConstants::AES_KEY_SIZE);
}

CryptoResult PacketCrypto::Encrypt(const char* plaintext, uint16_t size) {
    if (!initialized_ || !hKey_ || size == 0) {
        return CryptoResult::Failure("Crypto not initialized or empty payload");
    }

    UTILITY::LockGuard lock(crypto_mutex_);

    // 1. 시퀀스 번호 생성
    uint32_t seq = send_sequence_.fetch_add(1, std::memory_order_relaxed);

    // 2. 랜덤 IV 생성
    unsigned char iv[CryptoConstants::IV_SIZE];
    if (!GenerateRandomIV(iv, CryptoConstants::IV_SIZE)) {
        return CryptoResult::Failure("IV generation failed");
    }

    // IV를 복사 (BCryptEncrypt가 IV를 변경하므로)
    unsigned char iv_copy[CryptoConstants::IV_SIZE];
    std::memcpy(iv_copy, iv, CryptoConstants::IV_SIZE);

    // 3. 암호화 출력 크기 계산
    ULONG cipher_size = 0;
    NTSTATUS status = BCryptEncrypt(
        static_cast<BCRYPT_KEY_HANDLE>(hKey_),
        (PUCHAR)plaintext, size,
        nullptr, iv_copy, CryptoConstants::IV_SIZE,
        nullptr, 0, &cipher_size,
        BCRYPT_BLOCK_PADDING);
    if (!NT_SUCCESS(status)) {
        return CryptoResult::Failure("BCryptEncrypt size calculation failed");
    }

    // 4. 출력 버퍼 할당: [SeqNum(4)][IV(16)][CipherText]
    size_t total_size = CryptoConstants::SEQUENCE_NUM_SIZE + CryptoConstants::IV_SIZE + cipher_size;
    std::vector<char> output(total_size);

    // 시퀀스 번호 기록
    std::memcpy(output.data(), &seq, CryptoConstants::SEQUENCE_NUM_SIZE);

    // IV 기록 (암호화 전의 원본 IV)
    std::memcpy(output.data() + CryptoConstants::SEQUENCE_NUM_SIZE, iv, CryptoConstants::IV_SIZE);

    // 5. 암호화 실행
    // IV를 다시 복사 (BCryptEncrypt가 변경하므로)
    std::memcpy(iv_copy, iv, CryptoConstants::IV_SIZE);

    ULONG result_size = 0;
    status = BCryptEncrypt(
        static_cast<BCRYPT_KEY_HANDLE>(hKey_),
        (PUCHAR)plaintext, size,
        nullptr, iv_copy, CryptoConstants::IV_SIZE,
        (PUCHAR)(output.data() + CryptoConstants::SEQUENCE_NUM_SIZE + CryptoConstants::IV_SIZE),
        cipher_size, &result_size,
        BCRYPT_BLOCK_PADDING);

    if (!NT_SUCCESS(status)) {
        return CryptoResult::Failure("BCryptEncrypt failed");
    }

    // 실제 암호화된 크기로 조정
    output.resize(CryptoConstants::SEQUENCE_NUM_SIZE + CryptoConstants::IV_SIZE + result_size);
    return CryptoResult::Success(std::move(output));
}

CryptoResult PacketCrypto::Decrypt(const char* ciphertext, uint16_t size) {
    if (!initialized_ || !hKey_) {
        return CryptoResult::Failure("Crypto not initialized");
    }

    // 최소 크기 검증: SeqNum(4) + IV(16) + 최소 1블록(16)
    const uint16_t min_size = CryptoConstants::SEQUENCE_NUM_SIZE + CryptoConstants::IV_SIZE + CryptoConstants::AES_BLOCK_SIZE;
    if (size < min_size) {
        return CryptoResult::Failure("Encrypted payload too small");
    }

    UTILITY::LockGuard lock(crypto_mutex_);

    // 1. 시퀀스 번호 추출 및 검증
    uint32_t received_seq = 0;
    std::memcpy(&received_seq, ciphertext, CryptoConstants::SEQUENCE_NUM_SIZE);

    if (!ValidateSequence(received_seq)) {
        return CryptoResult::Failure("Sequence number validation failed (possible replay attack)");
    }

    // 2. IV 추출
    unsigned char iv[CryptoConstants::IV_SIZE];
    std::memcpy(iv, ciphertext + CryptoConstants::SEQUENCE_NUM_SIZE, CryptoConstants::IV_SIZE);

    // 3. 암호문 추출
    const char* encrypted_data = ciphertext + CryptoConstants::SEQUENCE_NUM_SIZE + CryptoConstants::IV_SIZE;
    uint16_t encrypted_size = size - CryptoConstants::SEQUENCE_NUM_SIZE - CryptoConstants::IV_SIZE;

    // 4. 복호화 출력 크기 계산
    ULONG plain_size = 0;
    NTSTATUS status = BCryptDecrypt(
        static_cast<BCRYPT_KEY_HANDLE>(hKey_),
        (PUCHAR)encrypted_data, encrypted_size,
        nullptr, iv, CryptoConstants::IV_SIZE,
        nullptr, 0, &plain_size,
        BCRYPT_BLOCK_PADDING);
    if (!NT_SUCCESS(status)) {
        return CryptoResult::Failure("BCryptDecrypt size calculation failed");
    }

    // 5. 복호화 실행
    // IV를 다시 세팅 (BCryptDecrypt가 IV를 변경)
    std::memcpy(iv, ciphertext + CryptoConstants::SEQUENCE_NUM_SIZE, CryptoConstants::IV_SIZE);

    std::vector<char> output(plain_size);
    ULONG result_size = 0;
    status = BCryptDecrypt(
        static_cast<BCRYPT_KEY_HANDLE>(hKey_),
        (PUCHAR)encrypted_data, encrypted_size,
        nullptr, iv, CryptoConstants::IV_SIZE,
        (PUCHAR)output.data(), plain_size, &result_size,
        BCRYPT_BLOCK_PADDING);

    if (!NT_SUCCESS(status)) {
        return CryptoResult::Failure("BCryptDecrypt failed");
    }

    output.resize(result_size);

    // 6. 시퀀스 번호 갱신
    recv_sequence_ = received_seq;

    return CryptoResult::Success(std::move(output));
}

bool PacketCrypto::ValidateSequence(uint32_t received_seq) {
    // 첫 패킷은 무조건 허용
    if (recv_sequence_ == 0 && received_seq == 0) return true;

    // 시퀀스가 현재보다 뒤에 있거나 윈도우 범위 내인지 확인
    // 리플레이 공격: 이미 처리한 시퀀스를 다시 보내는 경우 차단
    if (received_seq <= recv_sequence_) {
        // 오버플로 케이스 처리 (uint32_t 최대값 -> 0 순환)
        if (recv_sequence_ - received_seq < SEQUENCE_WINDOW) {
            return false; // 이미 처리한 시퀀스 (리플레이 의심)
        }
        // 오버플로로 인한 순환인 경우 허용
        return true;
    }

    // 시퀀스가 너무 큰 점프를 하면 의심 (선택적 검증)
    if (received_seq - recv_sequence_ > SEQUENCE_WINDOW * 10) {
        return false; // 비정상적인 시퀀스 점프
    }

    return true;
}

bool PacketCrypto::GenerateRandomIV(unsigned char* iv, size_t size) {
    NTSTATUS status = BCryptGenRandom(nullptr, iv, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return NT_SUCCESS(status);
}
