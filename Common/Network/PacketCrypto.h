#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <mutex>
#include "../Utils/Lock.h"

// ==========================================
//   패킷 암호화 유틸리티 (AES-128-CBC + 시퀀스 번호)
//
// [현재 문제]
//   TCP 평문 통신으로 패킷 스니핑에 완전 노출
//   리플레이 공격(동일 패킷 재전송) 방어 수단 없음
//
// [수정 후]
//   1. AES-128-CBC 대칭키 암호화로 패킷 페이로드 보호
//   2. 시퀀스 번호로 리플레이 공격 방지
//   3. Windows BCrypt API 사용 (OS 수준 암호 모듈)
//
// [암호화 패킷 구조]
//   [PacketHeader(4B)][SeqNum(4B)][IV(16B)][EncryptedPayload(N)]
//   - SeqNum: 단조 증가하는 4바이트 시퀀스 번호
//   - IV: AES-CBC 초기화 벡터 (매 패킷마다 랜덤 생성)
//   - EncryptedPayload: AES-128-CBC로 암호화된 원본 Protobuf 페이로드
//
// [사용 예]
//   PacketCrypto crypto;
//   crypto.Initialize(shared_key_16bytes);
//
//   // 송신 측
//   auto encrypted = crypto.Encrypt(payload_data, payload_size);
//
//   // 수신 측
//   auto decrypted = crypto.Decrypt(encrypted_data, encrypted_size);
//
// [주의]
//   현재 구현은 사전 공유 키(Pre-Shared Key) 방식입니다.
//   프로덕션 환경에서는 TLS 또는 Diffie-Hellman 키 교환을
//   통해 세션 키를 협상하는 것이 권장됩니다.
// ==========================================

// 암호화 관련 상수
namespace CryptoConstants {
    constexpr int AES_KEY_SIZE = 16;        // AES-128 키 크기 (바이트)
    constexpr int AES_BLOCK_SIZE = 16;      // AES 블록 크기 (바이트)
    constexpr int IV_SIZE = 16;             // 초기화 벡터 크기 (바이트)
    constexpr int SEQUENCE_NUM_SIZE = 4;    // 시퀀스 번호 크기 (바이트)

    // 암호화 오버헤드: SeqNum(4) + IV(16) + 패딩(최대 16)
    constexpr int MAX_CRYPTO_OVERHEAD = SEQUENCE_NUM_SIZE + IV_SIZE + AES_BLOCK_SIZE;

    // S2S 내부망 통신은 암호화를 건너뛰는 옵션
    constexpr bool ENCRYPT_S2S = false;
    constexpr bool ENCRYPT_CLIENT = true;
}

// 암호화/복호화 결과
struct CryptoResult {
    bool success;
    std::vector<char> data;
    std::string error_message;

    static CryptoResult Success(std::vector<char>&& d) {
        return { true, std::move(d), "" };
    }
    static CryptoResult Failure(const std::string& msg) {
        return { false, {}, msg };
    }
};

class PacketCrypto {
private:
    bool initialized_ = false;
    unsigned char key_[CryptoConstants::AES_KEY_SIZE] = { 0 };

    // 송신 시퀀스 번호 (단조 증가, 오버플로 시 자동 순환)
    std::atomic<uint32_t> send_sequence_{ 0 };

    // 수신 시퀀스 번호 (마지막으로 수신한 유효 시퀀스)
    uint32_t recv_sequence_ = 0;

    // 시퀀스 검증 시 허용하는 윈도우 크기
    // 네트워크 순서 뒤바뀜을 감안하여 약간의 여유를 둠
    static constexpr uint32_t SEQUENCE_WINDOW = 100;

    // BCrypt 핸들 (Windows)
    void* hAlgorithm_ = nullptr;
    void* hKey_ = nullptr;
    UTILITY::Lock crypto_mutex_; // BCrypt 핸들은 스레드 안전하지 않으므로 락 필요

public:
    PacketCrypto() = default;
    ~PacketCrypto();

    PacketCrypto(const PacketCrypto&) = delete;
    PacketCrypto& operator=(const PacketCrypto&) = delete;

    // AES-128 키로 초기화 (16바이트 키 필수)
    bool Initialize(const unsigned char* key, size_t key_len);

    // 사전 공유 키 문자열로 초기화 (SHA-256 해싱 후 앞 16바이트 사용)
    bool InitializeWithPassphrase(const std::string& passphrase);

    // 평문 페이로드를 암호화
    // 반환: [SeqNum(4B)][IV(16B)][EncryptedData]
    CryptoResult Encrypt(const char* plaintext, uint16_t size);

    // 암호화된 데이터를 복호화
    // 입력: [SeqNum(4B)][IV(16B)][EncryptedData]
    // 시퀀스 번호 검증 포함
    CryptoResult Decrypt(const char* ciphertext, uint16_t size);

    // 시퀀스 번호만 검증 (복호화 없이)
    bool ValidateSequence(uint32_t received_seq);

    bool IsInitialized() const { return initialized_; }

    // 랜덤 IV 생성 유틸리티
    static bool GenerateRandomIV(unsigned char* iv, size_t size);

    // 암호화 활성화 여부 (config 기반으로 확장 가능)
    static bool IsEncryptionEnabled() { return CryptoConstants::ENCRYPT_CLIENT; }
};
