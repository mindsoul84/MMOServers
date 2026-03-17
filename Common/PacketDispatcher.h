#pragma once
#include <functional>
#include <array>
#include <memory>
#include <iostream>

template <typename SessionType>
class PacketDispatcher {
public:
    // 핸들러 함수 시그니처 정의
    using HandlerFunc = std::function<void(std::shared_ptr<SessionType>&, char*, uint16_t)>;

private:
    // ========================================
    // 사용하는 패킷 ID만 동적으로 메모리 할당
    // ========================================
    std::unordered_map<uint16_t, HandlerFunc> handlers_;

public:
    // 패킷 핸들러 등록 (서버 초기화 시 메인 스레드에서만 호출되므로 락 불필요)
    void RegisterHandler(uint16_t pktId, HandlerFunc handler) {
        handlers_[pktId] = handler;
    }

    // 패킷 분배 (읽기 전용이므로 멀티스레드 환경에서도 안전함)
    void Dispatch(std::shared_ptr<SessionType>& session, uint16_t pktId, char* payload, uint16_t payloadSize) {
        auto it = handlers_.find(pktId);
        if (it != handlers_.end()) {
            if (it->second) {
                it->second(session, payload, payloadSize);
            }
        }
        else {
            std::cerr << "🚨 [PacketDispatcher] 등록되지 않은 패킷 ID 수신: " << pktId << "\n";
        }
    }
};