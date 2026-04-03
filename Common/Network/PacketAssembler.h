#pragma once
#include <cstdint>
#include <cstring>
#include "../MemoryPool.h"

// ==========================================
//   고정 크기 수신 버퍼 (Packet Assembler)
//
// [변경 전 문제]
//   std::vector<char> payload_buf_를 매 패킷마다 resize() 호출
//   -> header.size를 신뢰하여 그 크기만큼 동적 할당
//   -> 악의적 클라이언트가 MAX_PACKET_SIZE 이하의 반복적 할당 공격 가능
//   -> 힙 단편화 및 할당자 병목 유발
//
// [변경 후]
//   컴파일 타임에 MAX_PACKET_SIZE 크기로 고정 할당된 버퍼 사용
//   -> 런타임 동적 할당이 완전히 제거됨
//   -> 페이로드 크기 검증을 버퍼 레벨에서 강제
//   -> 캐시 친화적 메모리 레이아웃 (alignas(64))
//
// [사용 예]
//   class MySession {
//       PacketAssembler assembler_;
//       void ReadPayload(uint16_t size) {
//           if (!assembler_.ValidatePayloadSize(size)) { Disconnect(); return; }
//           async_read(socket_, assembler_.GetPayloadBuffer(size), ...);
//           // 수신 완료 후:
//           char* data = assembler_.GetPayloadData();
//       }
//   };
// ==========================================

class PacketAssembler {
private:
    // 캐시 라인 정렬로 성능 최적화 (false sharing 방지)
    alignas(64) char payload_buffer_[MAX_PACKET_SIZE];

public:
    PacketAssembler() {
        std::memset(payload_buffer_, 0, MAX_PACKET_SIZE);
    }

    // 페이로드 크기가 고정 버퍼 범위 내인지 검증
    // sizeof(PacketHeader)를 제외한 순수 페이로드 크기를 전달해야 함
    bool ValidatePayloadSize(uint16_t payload_size) const {
        return payload_size > 0 && payload_size <= (MAX_PACKET_SIZE - sizeof(uint16_t) * 2);
    }

    // boost::asio::async_read에 전달할 수신 버퍼 반환
    // 반드시 ValidatePayloadSize() 검증 후 호출할 것
    char* GetBuffer() { return payload_buffer_; }
    const char* GetBuffer() const { return payload_buffer_; }

    // 수신 완료된 페이로드 데이터 포인터 반환
    char* GetPayloadData() { return payload_buffer_; }
    const char* GetPayloadData() const { return payload_buffer_; }

    // 고정 버퍼의 최대 크기 반환
    static constexpr size_t GetMaxPayloadSize() {
        return MAX_PACKET_SIZE - sizeof(uint16_t) * 2;
    }

    // 복사/이동 금지 (세션에 1:1 귀속)
    PacketAssembler(const PacketAssembler&) = delete;
    PacketAssembler& operator=(const PacketAssembler&) = delete;
};
