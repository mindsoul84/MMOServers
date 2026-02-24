#pragma once
#include <functional>
#include <array>
#include <memory>
#include <iostream>

/*
[아키텍처 관점에서의 코드 분석]
std::array 기반 O(1) 조회: switch - case나 std::map은 패킷 종류가 많아지면 찾는 데 시간이 걸리거나(O(logN)) 코드가 무한정 길어집니다.
반면 이 방식은 _handlers[패킷ID] 형태로 메모리 주소에 즉시 접근하므로 서버 지연(Latency)을 최소화하는 실무 표준 기법입니다.
std::function(콜백 함수) : 클래스의 멤버 함수든, 전역 함수든, 람다(Lambda) 함수든 형태만 맞으면 모두 저장할 수 있는 강력한 C++11 기능입니다.
독립성 보장 : 이제 LoginServer는 메인 함수 시작 부분에서 dispatcher.RegisterHandler(1, LoginReq_Handler) 처럼 
자신에게 필요한 패킷만 딱딱 골라서 등록해 두면 됩니다.
*/

// 패킷 ID의 최대값 설정 (uint16_t의 최대값인 65535)
// 배열 기반 O(1) 조회를 위해 미리 공간을 할당합니다.
constexpr uint16_t MAX_PACKET_ID = UINT16_MAX;

// 템플릿을 사용하여 각 서버(Login, Game 등)의 고유한 Session 타입을 지원합니다.
template <typename SessionType>
class PacketDispatcher {
public:
    // 1. 핸들러 함수 시그니처 정의 (C++11 <functional> 활용)
    // [규칙] 반환형 void (세션 스마트 포인터, 페이로드 데이터, 페이로드 크기)
    using HandlerFunc = std::function<void(std::shared_ptr<SessionType>&, char*, uint16_t)>;

    PacketDispatcher() {
        // 배열의 모든 핸들러를 nullptr(빈 상태)로 초기화
        _handlers.fill(nullptr);
    }

    // 2. 패킷 핸들러 등록 (초기화 단계에서 사용)
    void RegisterHandler(uint16_t pktId, HandlerFunc handler) {
        if (pktId >= MAX_PACKET_ID) {
            std::cerr << "[Error] 패킷 ID가 최대 배열 크기를 초과했습니다: " << pktId << "\n";
            return;
        }
        _handlers[pktId] = handler;
    }

    // 3. 패킷 수신 시 호출 (핵심: O(1) 라우팅)
    bool Dispatch(std::shared_ptr<SessionType>& session, uint16_t pktId, char* payload, uint16_t payloadSize) {
        // 배열 범위를 벗어나거나, 등록되지 않은 패킷(nullptr)일 경우 방어
        if (pktId >= MAX_PACKET_ID || _handlers[pktId] == nullptr) {
            std::cout << "[Warning] 처리할 수 없거나 등록되지 않은 패킷 ID: " << pktId << "\n";
            return false;
        }

        // O(1) 배열 접근 후, 등록된 함수 포인터 즉시 실행!
        _handlers[pktId](session, payload, payloadSize);
        return true;
    }

private:
    // 함수 포인터들을 담아둘 거대한 배열 (메모리를 조금 쓰지만 속도는 압도적으로 빠릅니다)
    std::array<HandlerFunc, MAX_PACKET_ID> _handlers;
};