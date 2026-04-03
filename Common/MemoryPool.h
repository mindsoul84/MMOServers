#pragma once
#include <vector>
#include <memory>
#include <iostream>
#include <boost/lockfree/queue.hpp>

// ==========================================
//   메모리 풀 설정값 현실화
//
// 변경 전: MAX_PACKET_SIZE=65535, BUFFER_SIZE=10000
//   → 65535 × 10000 = 약 655MB 사전 할당 (LoginServer/WorldServer 600MB+ 점유 원인)
//
// 변경 후: MAX_PACKET_SIZE=4096 (실제 게임 패킷은 대부분 수백 바이트 이내)
//   → 풀 크기를 서버 역할별로 차등 적용 (Initialize 호출 시 결정)
//   → GameServer/GatewayServer: 10,000개 × 4KB = 약 40MB
//   → LoginServer/WorldServer:  1,000개 × 4KB = 약  4MB
// ==========================================
constexpr size_t MAX_PACKET_SIZE = 4096;

// 서버 역할별 풀 사전 할당 개수 상수
namespace PoolConfig {
    // GameServer/GatewayServer: AOI 브로드캐스트로 동시 전송량이 많음
    constexpr int HEAVY_SERVER = 10000;
    // LoginServer/WorldServer: S2S 통신 위주로 동시 전송량이 적음
    constexpr int LIGHT_SERVER = 1000;
}

// 재사용될 고정 크기 버퍼 객체
struct SendBuffer {
    std::vector<char> buffer_;
    SendBuffer(size_t exact_size) { buffer_.resize(exact_size); }
    SendBuffer() { buffer_.resize(MAX_PACKET_SIZE); }
};

// ==========================================
// Lock-Free 버퍼 풀 (오브젝트 풀)
//
//   지연 초기화(Lazy Init) 방식으로 변경
//   - 생성자에서 사전 할당하지 않음 (싱글톤 접근만으로 메모리 폭발 방지)
//   - 서버 main()에서 Initialize(count)를 호출하여 역할에 맞는 크기로 초기화
//   - Initialize 없이 Acquire 호출 시에도 동적 할당으로 안전하게 동작
// ==========================================
class SendBufferPool {
private:
    boost::lockfree::queue<SendBuffer*> pool_;
    bool initialized_ = false;

    //   생성자에서 사전 할당 제거 → Initialize()로 이관
    SendBufferPool() : pool_(128) {}

public:
    static SendBufferPool& GetInstance() {
        static SendBufferPool instance;
        return instance;
    }

    //     서버 역할에 맞는 풀 크기로 초기화 (main 시작부에서 1회 호출)
    void Initialize(int buffer_count) {
        if (initialized_) return;
        initialized_ = true;

        for (int i = 0; i < buffer_count; ++i) {
            pool_.push(new SendBuffer());
        }
        std::cout << "[SendBufferPool] 메모리 풀 초기화 완료: "
            << buffer_count << "개 x " << MAX_PACKET_SIZE
            << " bytes = 약 " << (static_cast<size_t>(buffer_count) * MAX_PACKET_SIZE / 1024 / 1024) << " MB\n";
    }

    // 버퍼 대여
    SendBuffer* Acquire() {
        SendBuffer* buf = nullptr;
        if (pool_.pop(buf)) {
            return buf; // 풀에서 즉시 꺼내줌 (Lock-Free)
        }
        // 풀이 고갈되었거나 Initialize 전이라면 임시 생성
        return new SendBuffer();
    }

    // 버퍼 반납
    void Release(SendBuffer* buf) {
        if (!pool_.push(buf)) {
            delete buf; // 풀이 꽉 찼을 때만 예외적으로 삭제
        }
    }
};

// ==========================================
//   스마트 포인터용 커스텀 딜리터 (Custom Deleter)
// ==========================================
// 비동기 전송이 끝난 후 delete 하는 대신, 풀에 반납하도록 가로채는 구조체입니다.
struct SendBufferDeleter {
    void operator()(SendBuffer* buf) const {
        SendBufferPool::GetInstance().Release(buf);
    }
};
