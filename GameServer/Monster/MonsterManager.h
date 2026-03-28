#pragma once
#include <memory>

class Monster;

// 몬스터 초기 스폰을 담당하는 함수
void InitMonsters();

// AI 몬스터들의 메인 게임 루프(심장)를 백그라운드 스레드로 가동하는 함수
void StartAITickThread();

// ==========================================
// [리팩토링] ScheduleNextAITick 내부 로직 함수 분리
//
// 변경 전: IDLE/CHASE/ATTACK/DEAD 처리, 리스폰, AOI 브로드캐스트까지
//          약 200줄이 하나의 타이머 콜백 람다에 들어있었음
//          -> 가독성 저하, 디버깅 난이도 증가, 락 순서 추적 어려움
//
// 변경 후: 상태별 처리를 독립 함수로 분리
//          -> 각 함수의 락 범위가 명확, 단위 테스트 가능
//          -> 락 획득 순서 규칙(monsterMutex_ -> playerMutex_)을 함수 단위로 검증 가능
// ==========================================

// 몬스터 사망 상태 처리 (리스폰 타이머, 부활 로직, AOI 알림)
void ProcessDeadMonster(std::shared_ptr<Monster>& mon, float delta_time);

// IDLE 상태 몬스터의 어그로 탐지 처리
void ProcessIdleMonster(std::shared_ptr<Monster>& mon, float old_x, float old_y);

// CHASE 상태 몬스터의 타겟 추적/포기 처리
void ProcessChaseMonster(std::shared_ptr<Monster>& mon, float old_x, float old_y);

// ATTACK 상태 몬스터의 타겟 유지/포기 처리
void ProcessAttackMonster(std::shared_ptr<Monster>& mon, float old_x, float old_y);

// 몬스터 이동 후 Zone 갱신 및 네트워크 동기화
void SyncMonsterPosition(std::shared_ptr<Monster>& mon, float old_x, float old_y, float delta_time);
