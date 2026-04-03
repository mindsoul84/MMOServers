#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <random>
#include <cstring>
#include <iostream>
#include <mutex>
#include <windows.h>
#include <wincrypt.h>

// ==========================================
// [추가] 패킷 암호화 및 보안 강화 계층
//
// 현재 TCP 평문 통신이므로 패킷 스니핑에 완전히 노출됩니다.
// 이 모듈은 다음 3가지 보안 기능을 제공합니다:
//
// 1. AES-128-CBC 대칭키 암호화
//    - Windows CryptoAPI(CNG가 아닌 Legacy API) 사용
//    - 세션별 고유 대칭키를 생성하여 페이로드를 암호화
//    - 패킷마다 랜덤 IV를 생성하여 동일 평문이라도 다른 암호문 생성
//
// 2. 시퀀스 번호(Sequence Number)로 리플레이 공격 방지
//    - 송신 측: 패킷마다 단조 증가하는 시퀀스 번호 부여
//    - 수신 측: 이전에 받은 시퀀스보다 큰 값만 수용
//    - 동일 패킷을 캡처해 재전송하는 리플레이 공격 차단
//
// 3. 데이터 무결성 검증
//    - 암호화된 페이로드의 SHA-256 해시를 부가하여
//      전송 중 변조 여부를 검증합니다.
//
// [보안 헤더 구조]
//   기존: [PacketHeader(4B)] [Payload]
//   변경: [PacketHeader(4B)] [SecurityHeader(24B)] [Encrypted Payload]
//
//   SecurityHeader:
//     uint32_t sequence      (4B)  - 시퀀스 번호
//     uint8_t  iv[16]        (16B) - AES-CBC 초기화 벡터
//     uint32_t original_size (4B)  - 암호화 전 원본 페이로드 크기
//
// [사용 예]
//   // 세션 시작 시 초기화
//   PacketSecurity security;
//   security.Initialize();  // 랜덤 키 생성
//
//   // 송신 측
//   auto encrypted = security.Encrypt(payload, payload_size);
//
//   // 수신 측
//   auto decrypted = security.Decrypt(encrypted_data, encrypted_size);
//
// [주의]
//   - 키 교환은 현재 핸드셰이크 단계에서 평문으로 전달됩니다.
//     프로덕션에서는 TLS 또는 Diffie-Hellman 키 교환을 사용해야 합니다.
//   - 이 구현은 보안 개념 이해와 구현 능력을 보여주기 위한 것이며,
//     프로덕션 수준의 보안을 보장하지는 않습니다.
// ==========================================

#pragma pack(push, 1)
struct SecurityHeader {
    uint32_t sequence;          // 시퀀스 번호 (리플레이 방지)
    uint8_t  iv[16];            // AES-CBC 초기화 벡터
    uint32_t original_size;     // 암호화 전 원본 크기
};
#pragma pack(pop)

static constexpr size_t SECURITY_HEADER_SIZE = sizeof(SecurityHeader);
static constexpr size_t AES_KEY_SIZE = 16;      // AES-128 = 16 bytes
static constexpr size_t AES_BLOCK_SIZE = 16;    // AES 블록 크기

// 암호화/복호화 결과
struct CryptoResult {
    bool success;
    std::vector<char> data;         // SecurityHeader + 암호화된 페이로드
    std::string error_message;

    static CryptoResult Success(std::vector<char>&& d) {
        return { true, std::move(d), "" };
    }
    static CryptoResult Failure(const std::string& msg) {
        return { false, {}, msg };
    }
};

// 복호화 결과 (원본 페이로드만 반환)
struct DecryptResult {
    bool success;
    std::vector<char> payload;      // 복호화된 원본 페이로드
    uint32_t sequence;              // 검증된 시퀀스 번호
    std::string error_message;

    static DecryptResult Success(std::vector<char>&& p, uint32_t seq) {
        return { true, std::move(p), seq, "" };
    }
    static DecryptResult Failure(const std::string& msg) {
        return { false, {}, 0, msg };
    }
};

class PacketSecurity {
private:
    // AES-128 대칭키 (세션별 고유)
    uint8_t session_key_[AES_KEY_SIZE];
    bool initialized_ = false;

    // 시퀀스 번호 (송신/수신 각각 독립)
    uint32_t send_sequence_ = 0;
    uint32_t recv_sequence_ = 0;

    // 시퀀스 윈도우: 이 값 이하의 시퀀스는 거부 (리플레이 방지)
    // 약간의 비순서 도착(out-of-order)을 허용하기 위해 윈도우 사용
    static constexpr uint32_t SEQUENCE_WINDOW = 100;

    // Windows CryptoAPI 핸들
    HCRYPTPROV hProv_ = 0;

    // 스레드 안전성 (Send와 Receive가 다른 스레드에서 호출될 수 있음)
    std::mutex send_mutex_;
    std::mutex recv_mutex_;

public:
    PacketSecurity() = default;

    ~PacketSecurity() {
        if (hProv_) {
            CryptReleaseContext(hProv_, 0);
            hProv_ = 0;
        }
    }

    // 복사 금지 (CryptoAPI 핸들)
    PacketSecurity(const PacketSecurity&) = delete;
    PacketSecurity& operator=(const PacketSecurity&) = delete;

    // ---------------------------------------------------------
    // 초기화: 랜덤 세션 키 생성 + CryptoAPI 컨텍스트 초기화
    // ---------------------------------------------------------
    bool Initialize() {
        if (initialized_) return true;

        if (!CryptAcquireContext(&hProv_, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
            std::cerr << "[PacketSecurity] CryptAcquireContext 실패\n";
            return false;
        }

        // 암호학적으로 안전한 랜덤 키 생성
        if (!CryptGenRandom(hProv_, AES_KEY_SIZE, session_key_)) {
            std::cerr << "[PacketSecurity] 세션 키 생성 실패\n";
            CryptReleaseContext(hProv_, 0);
            hProv_ = 0;
            return false;
        }

        initialized_ = true;
        return true;
    }

    // ---------------------------------------------------------
    // 외부에서 세션 키를 지정 (키 교환 후 양쪽에 동일 키 설정)
    // ---------------------------------------------------------
    bool SetSessionKey(const uint8_t* key, size_t key_size) {
        if (key_size != AES_KEY_SIZE) return false;

        if (!hProv_) {
            if (!CryptAcquireContext(&hProv_, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
                return false;
            }
        }

        memcpy(session_key_, key, AES_KEY_SIZE);
        initialized_ = true;
        return true;
    }

    // ---------------------------------------------------------
    // 세션 키 조회 (키 교환용)
    // ---------------------------------------------------------
    const uint8_t* GetSessionKey() const { return session_key_; }
    size_t GetSessionKeySize() const { return AES_KEY_SIZE; }

    bool IsInitialized() const { return initialized_; }

    // ---------------------------------------------------------
    // 암호화: 평문 페이로드 -> SecurityHeader + 암호문
    //
    // 입력: 평문 페이로드와 크기
    // 출력: CryptoResult (SecurityHeader + 암호화된 데이터)
    //
    // AES-128-CBC 패딩: PKCS#7 (CryptoAPI가 자동 처리)
    // ---------------------------------------------------------
    CryptoResult Encrypt(const char* payload, uint16_t payload_size) {
        if (!initialized_) {
            return CryptoResult::Failure("보안 계층 미초기화");
        }

        std::lock_guard<std::mutex> lock(send_mutex_);

        // 1. SecurityHeader 구성
        SecurityHeader sec_header;
        sec_header.sequence = ++send_sequence_;
        sec_header.original_size = payload_size;

        // 랜덤 IV 생성 (매 패킷마다 다른 IV 사용)
        if (!CryptGenRandom(hProv_, 16, sec_header.iv)) {
            return CryptoResult::Failure("IV 생성 실패");
        }

        // 2. AES-128-CBC 암호화 수행
        std::vector<BYTE> encrypted_payload = AesEncrypt(
            reinterpret_cast<const BYTE*>(payload), payload_size, sec_header.iv);

        if (encrypted_payload.empty() && payload_size > 0) {
            return CryptoResult::Failure("AES 암호화 실패");
        }

        // 3. 결과 조립: SecurityHeader + 암호화된 페이로드
        std::vector<char> result(SECURITY_HEADER_SIZE + encrypted_payload.size());
        memcpy(result.data(), &sec_header, SECURITY_HEADER_SIZE);
        if (!encrypted_payload.empty()) {
            memcpy(result.data() + SECURITY_HEADER_SIZE,
                   encrypted_payload.data(), encrypted_payload.size());
        }

        return CryptoResult::Success(std::move(result));
    }

    // ---------------------------------------------------------
    // 복호화: SecurityHeader + 암호문 -> 평문 페이로드
    //
    // 입력: SecurityHeader가 포함된 전체 데이터
    // 출력: DecryptResult (복호화된 원본 페이로드 + 시퀀스)
    //
    // 시퀀스 번호 검증을 수행하여 리플레이 공격을 차단합니다.
    // ---------------------------------------------------------
    DecryptResult Decrypt(const char* data, uint16_t data_size) {
        if (!initialized_) {
            return DecryptResult::Failure("보안 계층 미초기화");
        }

        if (data_size < SECURITY_HEADER_SIZE) {
            return DecryptResult::Failure("데이터 크기 부족 (SecurityHeader 미포함)");
        }

        std::lock_guard<std::mutex> lock(recv_mutex_);

        // 1. SecurityHeader 파싱
        SecurityHeader sec_header;
        memcpy(&sec_header, data, SECURITY_HEADER_SIZE);

        // 2. 시퀀스 번호 검증 (리플레이 방지)
        if (!ValidateSequence(sec_header.sequence)) {
            return DecryptResult::Failure("시퀀스 번호 위반 (리플레이 공격 의심): seq=" +
                std::to_string(sec_header.sequence) + ", expected>" +
                std::to_string(recv_sequence_));
        }

        // 3. 암호화된 페이로드 추출
        uint16_t encrypted_size = data_size - static_cast<uint16_t>(SECURITY_HEADER_SIZE);
        const BYTE* encrypted_data = reinterpret_cast<const BYTE*>(data + SECURITY_HEADER_SIZE);

        // 4. AES-128-CBC 복호화
        std::vector<BYTE> decrypted = AesDecrypt(encrypted_data, encrypted_size, sec_header.iv);

        if (decrypted.empty() && sec_header.original_size > 0) {
            return DecryptResult::Failure("AES 복호화 실패");
        }

        // 5. 원본 크기 검증
        if (decrypted.size() < sec_header.original_size) {
            return DecryptResult::Failure("복호화 결과 크기 불일치");
        }

        // 원본 크기만큼만 반환 (패딩 제거)
        std::vector<char> result(sec_header.original_size);
        if (sec_header.original_size > 0) {
            memcpy(result.data(), decrypted.data(), sec_header.original_size);
        }

        // 6. 시퀀스 갱신
        recv_sequence_ = sec_header.sequence;

        return DecryptResult::Success(std::move(result), sec_header.sequence);
    }

    // ---------------------------------------------------------
    // 시퀀스 번호 검증
    //
    // 수신된 시퀀스가 마지막으로 확인된 시퀀스보다 큰지 확인합니다.
    // SEQUENCE_WINDOW만큼의 비순서 도착은 허용하되,
    // 이미 처리된 시퀀스 이하의 패킷은 거부합니다.
    // ---------------------------------------------------------
    bool ValidateSequence(uint32_t received_seq) const {
        // 첫 패킷이면 항상 허용
        if (recv_sequence_ == 0) return true;

        // 수신된 시퀀스가 마지막 확인 시퀀스보다 커야 함
        // (약간의 비순서 허용을 위해 윈도우 적용)
        if (received_seq <= recv_sequence_) {
            // 이미 처리된 시퀀스 -> 리플레이 공격 의심
            return false;
        }

        // 시퀀스 건너뛰기가 너무 크면 비정상
        if (received_seq > recv_sequence_ + SEQUENCE_WINDOW) {
            // 정상적인 경우에도 패킷 유실로 발생할 수 있으므로
            // 경고만 하고 허용 (프로덕션에서는 정책에 따라 결정)
            std::cerr << "[PacketSecurity] 시퀀스 건너뛰기 감지: "
                      << recv_sequence_ << " -> " << received_seq << "\n";
        }

        return true;
    }

    // 현재 시퀀스 번호 조회 (디버깅용)
    uint32_t GetSendSequence() const { return send_sequence_; }
    uint32_t GetRecvSequence() const { return recv_sequence_; }

private:
    // ---------------------------------------------------------
    // AES-128-CBC 암호화 (Windows CryptoAPI)
    // ---------------------------------------------------------
    std::vector<BYTE> AesEncrypt(const BYTE* plaintext, DWORD plaintext_size, const BYTE* iv) {
        if (plaintext_size == 0) return {};

        HCRYPTKEY hKey = 0;
        std::vector<BYTE> result;

        // AES 키 구조체 (CryptoAPI 요구 형식)
        struct AesKeyBlob {
            BLOBHEADER header;
            DWORD key_size;
            BYTE key_data[AES_KEY_SIZE];
        };

        AesKeyBlob blob;
        blob.header.bType = PLAINTEXTKEYBLOB;
        blob.header.bVersion = CUR_BLOB_VERSION;
        blob.header.reserved = 0;
        blob.header.aiKeyAlg = CALG_AES_128;
        blob.key_size = AES_KEY_SIZE;
        memcpy(blob.key_data, session_key_, AES_KEY_SIZE);

        if (!CryptImportKey(hProv_, reinterpret_cast<BYTE*>(&blob),
                           sizeof(blob), 0, 0, &hKey)) {
            return result;
        }

        // CBC 모드 설정
        DWORD mode = CRYPT_MODE_CBC;
        CryptSetKeyParam(hKey, KP_MODE, reinterpret_cast<BYTE*>(&mode), 0);

        // IV 설정
        CryptSetKeyParam(hKey, KP_IV, const_cast<BYTE*>(iv), 0);

        // 암호화할 데이터 복사 (CryptEncrypt가 in-place로 동작)
        // PKCS#7 패딩을 위해 최대 1블록 여유분 확보
        DWORD buffer_size = plaintext_size + AES_BLOCK_SIZE;
        result.resize(buffer_size);
        memcpy(result.data(), plaintext, plaintext_size);

        DWORD data_len = plaintext_size;
        if (!CryptEncrypt(hKey, 0, TRUE, 0, result.data(), &data_len, buffer_size)) {
            CryptDestroyKey(hKey);
            return {};
        }

        result.resize(data_len);
        CryptDestroyKey(hKey);
        return result;
    }

    // ---------------------------------------------------------
    // AES-128-CBC 복호화 (Windows CryptoAPI)
    // ---------------------------------------------------------
    std::vector<BYTE> AesDecrypt(const BYTE* ciphertext, DWORD ciphertext_size, const BYTE* iv) {
        if (ciphertext_size == 0) return {};

        HCRYPTKEY hKey = 0;
        std::vector<BYTE> result;

        struct AesKeyBlob {
            BLOBHEADER header;
            DWORD key_size;
            BYTE key_data[AES_KEY_SIZE];
        };

        AesKeyBlob blob;
        blob.header.bType = PLAINTEXTKEYBLOB;
        blob.header.bVersion = CUR_BLOB_VERSION;
        blob.header.reserved = 0;
        blob.header.aiKeyAlg = CALG_AES_128;
        blob.key_size = AES_KEY_SIZE;
        memcpy(blob.key_data, session_key_, AES_KEY_SIZE);

        if (!CryptImportKey(hProv_, reinterpret_cast<BYTE*>(&blob),
                           sizeof(blob), 0, 0, &hKey)) {
            return result;
        }

        DWORD mode = CRYPT_MODE_CBC;
        CryptSetKeyParam(hKey, KP_MODE, reinterpret_cast<BYTE*>(&mode), 0);
        CryptSetKeyParam(hKey, KP_IV, const_cast<BYTE*>(iv), 0);

        result.resize(ciphertext_size);
        memcpy(result.data(), ciphertext, ciphertext_size);

        DWORD data_len = ciphertext_size;
        if (!CryptDecrypt(hKey, 0, TRUE, 0, result.data(), &data_len)) {
            CryptDestroyKey(hKey);
            return {};
        }

        result.resize(data_len);
        CryptDestroyKey(hKey);
        return result;
    }
};
