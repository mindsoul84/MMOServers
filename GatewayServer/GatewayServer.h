#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>
#include <chrono>

#include "..\Common\Protocol\protocol.pb.h"
#include "PacketDispatcher.h"

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t size;
    uint16_t id;
};
#pragma pack(pop)

class GameConnection;
class ClientSession;

// ==========================================
// [수정] GatewayContext 싱글톤 → 의존성 주입(DI) 지원
//
// 변경 전: private 생성자 + static Get() → 테스트 불가
// 변경 후: public 생성자 + SetTestInstance() → 테스트에서 목(mock) 주입 가능
//
// [테스트 코드 사용 예]
//   GatewayContext testCtx;
//   GatewayContext::SetTestInstance(&testCtx);
//   // ... 테스트 수행 ...
//   GatewayContext::SetTestInstance(nullptr); // 복원
// [추가] 대기 중인 토큰 엔트리
//
// WorldServer에서 발급된 토큰이 S2S 경로(World->Game->Gateway)를 통해
// 도착하면 이 구조체에 저장됩니다. 클라이언트 접속 시 검증에 사용됩니다.
// ==========================================
struct PendingToken {
    std::string session_token;
    int64_t expire_time_ms;
};

struct GatewayContext {
    PacketDispatcher<ClientSession>  clientDispatcher;
    PacketDispatcher<GameConnection> gameDispatcher;

    std::shared_ptr<GameConnection> gameConnection;

    std::unordered_map<std::string, std::shared_ptr<ClientSession>> clientMap;
    std::mutex clientMutex;

    

    // [추가] 세션 토큰 검증용 저장소
    // Key: account_id, Value: PendingToken
    // WorldServer -> GameServer -> GatewayServer 경로로 전달받은 토큰을 보관
    std::unordered_map<std::string, PendingToken> pendingTokens;
    std::mutex tokenMutex;

    // [추가] 토큰 검증 함수
    bool VerifyAndConsumeToken(const std::string& account_id, const std::string& token) {
        std::lock_guard<std::mutex> lock(tokenMutex);
        auto it = pendingTokens.find(account_id);
        if (it == pendingTokens.end()) return false;

        // 만료 시각 확인
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (it->second.expire_time_ms < now_ms) {
            pendingTokens.erase(it);
            return false;
        }

        // 토큰 비교
        if (it->second.session_token != token) return false;

        // 일회용: 검증 성공 후 삭제
        pendingTokens.erase(it);
        return true;
    }

    // [추가] 토큰 저장 함수
    void StorePendingToken(const std::string& account_id, const std::string& token, int64_t expire_ms) {
        std::lock_guard<std::mutex> lock(tokenMutex);
        pendingTokens[account_id] = { token, expire_ms };
    }

    // [수정] 테스트 인스턴스 오버라이드 포인터
    // [수정] inline 정의 (C++17) → 별도 .cpp 없이 링크 완결
    inline static GatewayContext* s_test_instance_ = nullptr;

    // [수정] 테스트 인스턴스가 주입된 경우 반환, 없으면 정적 싱글톤 반환
    static GatewayContext& Get() {
        if (s_test_instance_) return *s_test_instance_;
        static GatewayContext instance;
        return instance;
    }

    // [수정] 테스트 전용 주입 메서드
    static void SetTestInstance(GatewayContext* instance) noexcept {
        s_test_instance_ = instance;
    }

    // [수정] public 생성자 → 테스트 코드에서 직접 인스턴스 생성 가능
    GatewayContext() = default;

    GatewayContext(const GatewayContext&) = delete;
    GatewayContext& operator=(const GatewayContext&) = delete;
};
