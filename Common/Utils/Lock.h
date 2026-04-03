#pragma once

#include <mutex>

// ==========================================
//   std::mutex 기반으로 재정의
//
// 변경 전: std::shared_timed_mutex 기반 (ReadLock/WriteLock 분리)
//   -> game_strand_ 도입 후 읽기/쓰기 락 구분이 불필요해짐
//   -> 모든 임계 구역이 마이크로초 이하로 짧아 shared_mutex 오버헤드만 발생
//   -> shared_timed_mutex의 try_lock_for 기능을 사용하는 코드가 전혀 없음
//
// 변경 후: std::mutex 기반 단일 배타 락으로 통일
//   -> 프로젝트 전체에서 동일한 락 타입 사용 (일관성 확보)
//   -> 향후 락 구현체 교체 시 이 파일만 수정하면 전체 반영
//
//   UTILITY::Lock       = std::mutex                     (뮤텍스 타입)
//   UTILITY::LockGuard  = std::lock_guard<std::mutex>    (스코프 락, RAII)
//   UTILITY::UniqueLock = std::unique_lock<std::mutex>   (조건 변수 대기용)
// ==========================================

namespace UTILITY
{
    using Lock = std::mutex;
    using LockGuard = std::lock_guard<Lock>;
    using UniqueLock = std::unique_lock<Lock>;

} //namespace UTILITY
