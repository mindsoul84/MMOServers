#pragma once
#include <vector>
#include <memory>
#include <boost/lockfree/queue.hpp>

constexpr size_t MAX_PACKET_SIZE = 65535;
constexpr int    BUFFER_SIZE = 10000;

// 재사용될 고정 크기 버퍼 객체
struct SendBuffer {
    std::vector<char> buffer_;
    SendBuffer(size_t exact_size) { buffer_.resize(exact_size); }
    SendBuffer() { buffer_.resize(MAX_PACKET_SIZE); }
};

// ==========================================
// ★ Lock-Free 버퍼 풀 (오브젝트 풀)
// ==========================================
class SendBufferPool {
private:
    // Boost의 Lock-Free Queue를 사용하여 병목을 완벽히 제거
    boost::lockfree::queue<SendBuffer*> pool_;

    SendBufferPool() : pool_(BUFFER_SIZE) { // 큐 용량 1만 개 설정
        // 서버 시작 시 미리 1만 개의 버퍼를 힙에 올려둠 (Pre-allocation)
        for (int i = 0; i < BUFFER_SIZE; ++i) {
            pool_.push(new SendBuffer());
        }
    }

public:
    static SendBufferPool& GetInstance() {
        static SendBufferPool instance;
        return instance;
    }

    // 버퍼 대여
    SendBuffer* Acquire() {
        SendBuffer* buf = nullptr;
        if (pool_.pop(buf)) {
            return buf; // 풀에서 즉시 꺼내줌 (Lock-Free)
        }
        // 풀이 고갈되었다면 어쩔 수 없이 임시 생성
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
// ★ 스마트 포인터용 커스텀 딜리터 (Custom Deleter)
// ==========================================
// 비동기 전송이 끝난 후 delete 하는 대신, 풀에 반납하도록 가로채는 마법의 구조체입니다.
struct SendBufferDeleter {
    void operator()(SendBuffer* buf) const {
        SendBufferPool::GetInstance().Release(buf);
    }
};