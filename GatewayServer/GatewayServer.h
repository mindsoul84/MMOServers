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
// ★ [전역 변수 과남용 수정] GatewayContext 클래스
// ==========================================
// ❌ 기존 방식: 전역 변수 + extern 선언이 여러 파일에 흩어져 있어
//              어디서든 아무 파일에서나 접근 가능 → 의도치 않은 수정, 관리 불가
//
// ✅ 수정 방식: 게이트웨이 서버가 필요로 하는 상태를 하나의 구조체로 묶어
//              접근 경로를 명확히 하고 싱글톤으로 단일 진입점 제공
//
// 장점:
//   1. 헤더만 봐도 게이트웨이의 전체 상태가 한눈에 파악됨
//   2. extern 남발 제거 → 컴파일 의존성 감소
//   3. 추후 멀티 월드/채널 확장 시 Context를 복수로 만드는 것이 쉬워짐
// ==========================================
struct GatewayContext {
    // 패킷 디스패처 (클라이언트 방향 / 게임서버 방향)
    PacketDispatcher<ClientSession>  clientDispatcher;
    PacketDispatcher<GameConnection> gameDispatcher;

    // S2S 게임서버 연결 (일반적으로 1개)
    std::shared_ptr<GameConnection> gameConnection;

    // 접속 중인 클라이언트 세션 맵 (account_id → session)
    std::unordered_map<std::string, std::shared_ptr<ClientSession>> clientMap;
    std::mutex clientMutex;

    // 싱글톤 접근
    static GatewayContext& Get() {
        static GatewayContext instance;
        return instance;
    }

    // 복사/이동 금지
    GatewayContext(const GatewayContext&) = delete;
    GatewayContext& operator=(const GatewayContext&) = delete;

private:
    GatewayContext() = default;
};