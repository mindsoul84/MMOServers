#pragma once
#include "..\PathFinder\PathFinder.h"
#include <vector>
#include <cstdint>
#include <functional> // [추가] 콜백 함수 사용을 위함

// 몬스터의 상태 정의
enum class MonsterState {
    IDLE,
    CHASE,
    ATTACK,
    RETURN // [추가] 타겟을 놓치면 제자리로 돌아가는 상태
};

class Monster {
private:
    uint64_t monster_id_;
    Vector3 position_;
    Vector3 spawn_position_; // [추가] 몬스터가 원래 태어난 고향 좌표
    MonsterState state_;
    NavMesh* navmesh_; // Zone 서버가 맵 생성 시 주입해 주는 길찾기 포인터

    // 타겟(유저) 정보
    uint64_t target_user_id_;
    Vector3 target_last_pos_;    
    std::vector<Vector3> current_path_;     // 현재 이동 경로 데이터
    int path_index_;

    // [전투용 스탯 추가]
    int hp_;
    int max_hp_;
    int attack_power_;
    float attack_range_;      // 공격 사거리 (예: 1.5f)
    float attack_cooldown_;   // 공격 주기 (예: 2.0초)
    float attack_timer_;      // 쿨타임 계산용 타이머

    // [콜백 추가] 몬스터가 공격 모션을 취할 때 호출될 함수
    std::function<void(uint64_t attacker_id, uint64_t target_id, int damage)> on_attack_callback_;

public:
    // 생성자 선언
    Monster(uint64_t id, NavMesh* navmesh);

    // 추가: GameServer가 몬스터의 상태를 알기 위한 Getter
    uint64_t GetId() const { return monster_id_; }
    Vector3 GetPosition() const { return position_; }
    void SetPosition(float x, float y, float z) { position_ = { x, y, z }; }

    // [추가] 고향 좌표 세팅용 Getter/Setter
    void SetSpawnPosition(float x, float y, float z) { spawn_position_ = { x, y, z }; }
    Vector3 GetSpawnPosition() const { return spawn_position_; }

    // [추가] 현재 상태 반환 및 타겟 강제 지정 함수
    MonsterState GetState() const { return state_; }
    void SetTarget(uint64_t target_id, Vector3 target_pos);

    // [추가] 추적 유지를 위한 새로운 함수들
    uint64_t GetTargetUserId() const { return target_user_id_; }
    void UpdateTargetPosition(Vector3 new_pos); // 타겟이 도망가면 목적지 갱신
    void GiveUpChase();
    void GiveUpAttack();

    // [추가] 공격 콜백 등록 함수
    void SetOnAttackCallback(std::function<void(uint64_t, uint64_t, int)> cb) {
        on_attack_callback_ = cb;
    }

    // 서버 Tick 마다 호출될 업데이트 함수 선언
    void Update(float delta_time);

private:
    // 각 상태별 처리 함수 선언
    void UpdateIdle();
    void UpdateChase(float delta_time);
    void UpdateReturn(float delta_time); // [추가] 복귀 로직 함수
    void UpdateAttack(float delta_time); // [수정] delta_time 파라미터 추가

    // 내부 길찾기 호출 함수 선언
    void CalculatePath();
};