#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>

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
// ★ [수정 1] GatewayContext 싱글톤 → 의존성 주입(DI) 지원
//
// 변경 전: private 생성자 + static Get() → 테스트 불가
// 변경 후: public 생성자 + SetTestInstance() → 테스트에서 목(mock) 주입 가능
//
// [테스트 코드 사용 예]
//   GatewayContext testCtx;
//   GatewayContext::SetTestInstance(&testCtx);
//   // ... 테스트 수행 ...
//   GatewayContext::SetTestInstance(nullptr); // 복원
// ==========================================
struct GatewayContext {
    PacketDispatcher<ClientSession>  clientDispatcher;
    PacketDispatcher<GameConnection> gameDispatcher;

    std::shared_ptr<GameConnection> gameConnection;

    std::unordered_map<std::string, std::shared_ptr<ClientSession>> clientMap;
    std::mutex clientMutex;

    // ★ [수정 1] 테스트 인스턴스 오버라이드 포인터
    // ★ [수정] inline 정의 (C++17) → 별도 .cpp 없이 링크 완결
    inline static GatewayContext* s_test_instance_ = nullptr;

    // ★ [수정 1] 테스트 인스턴스가 주입된 경우 반환, 없으면 정적 싱글톤 반환
    static GatewayContext& Get() {
        if (s_test_instance_) return *s_test_instance_;
        static GatewayContext instance;
        return instance;
    }

    // ★ [추가 - 수정 1] 테스트 전용 주입 메서드
    static void SetTestInstance(GatewayContext* instance) noexcept {
        s_test_instance_ = instance;
    }

    // ★ [수정 1] public 생성자 → 테스트 코드에서 직접 인스턴스 생성 가능
    GatewayContext() = default;

    GatewayContext(const GatewayContext&) = delete;
    GatewayContext& operator=(const GatewayContext&) = delete;
};
