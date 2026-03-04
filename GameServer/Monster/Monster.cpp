#include "Monster.h"
#include <iostream>

// 생성자 구현
Monster::Monster(uint64_t id, NavMesh* navmesh)
    : monster_id_(id), state_(MonsterState::IDLE), navmesh_(navmesh),
    target_user_id_(0), path_index_(0),
    hp_(100), max_hp_(100), attack_power_(15), // 기본 공격력 15,
    defense_power_(10), // 기본 방어력 10
    attack_range_(0.5f), attack_cooldown_(2.0f), attack_timer_(2.0f) { // 첫 타격은 즉시 때리도록 타이머를 꽉 채워둠
    position_ = { 0.0f, 0.0f, 0.0f };
}

// Tick 업데이트 구현
void Monster::Update(float delta_time) {
    switch (state_) {
    case MonsterState::IDLE:
        UpdateIdle();
        break;
    case MonsterState::CHASE:
        UpdateChase(delta_time);
        break;
    case MonsterState::RETURN:   // [추가] 복귀 상태 분기
        UpdateReturn(delta_time);
        break;
    case MonsterState::ATTACK:
        UpdateAttack(delta_time); // 파라미터 추가
        break;
    }
}

// IDLE 상태 로직
void Monster::UpdateIdle() {
    // [로직] 주변(AOI)에 유저가 있는지 검색한다.
    // 발견했다면? 타겟을 설정하고 CHASE 상태로 변경!
    /*
    target_user_id_ = 발견한_유저_ID;
    target_last_pos_ = 유저의_현재_좌표;
    state_ = MonsterState::CHASE;
    CalculatePath(); // 길찾기 시작
    */
}

// ==========================================
// [추가] 외부에서 타겟을 지정받았을 때의 처리
// ==========================================
void Monster::SetTarget(uint64_t target_id, Vector3 target_pos) {
    target_user_id_ = target_id;
    target_last_pos_ = target_pos;

    // 이미 CHASE 상태가 아닐 때만 로그를 띄웁니다 (로그 도배 방지)
    if (state_ != MonsterState::CHASE) {
        state_ = MonsterState::CHASE;
        std::cout << "[Monster " << monster_id_ << "] 🚨 유저(" << target_id << ") 발견! 추적(CHASE) 모드 가동!\n";
    }
    CalculatePath();
}

// [추가] 타겟이 1칸 범위 내에서 움직였을 때 목적지를 갱신하는 함수
void Monster::UpdateTargetPosition(Vector3 new_pos) {
    float dx = target_last_pos_.x - new_pos.x;
    float dy = target_last_pos_.y - new_pos.y;

    // 유저가 예전 목적지에서 조금이라도 움직였다면 새 경로 계산
    if (std::sqrt(dx * dx + dy * dy) > 0.1f) {
        target_last_pos_ = new_pos;
        CalculatePath();

        // [로그 추가] 유저가 이동한 바로 그 시점에, 몬스터가 어디에 있는지 출력합니다!
        std::cout << "[Monster " << monster_id_ << "] 🏃 유저 이동(도착지 X:" << new_pos.x << ", Y:" << new_pos.y
            << ") -> 현재 몬스터 위치(X:" << position_.x << ", Y:" << position_.y << ")에서 추격 중!\n";
    }
}

// [추가] 유저가 너무 멀리 도망갔을 때 추적을 포기하는 함수
void Monster::GiveUpChase() {
    if (state_ == MonsterState::CHASE) {
        std::cout << "[Monster " << monster_id_ << "] 💨 거리가 멀어져 타겟을 놓쳤습니다. 제자리로 복귀(RETURN)합니다.\n";
        state_ = MonsterState::RETURN;
        target_last_pos_ = spawn_position_;
        CalculatePath(); // 고향으로 길찾기!
    }
}

void Monster::GiveUpAttack() {
    state_ = MonsterState::RETURN;
    target_user_id_ = 0; // 타겟 초기화

    // 공격 포기 전용 로그 출력
    std::cout << "[Monster " << monster_id_ << "] 🛑 타겟을 잃었습니다! 공격을 중지하고 고향으로 복귀(RETURN)합니다.\n";

    // 타겟 좌표를 고향으로 맞추고 경로 계산
    target_last_pos_ = spawn_position_;
    CalculatePath();
}

// ==========================================
// CHASE 상태 로직 수정 (추적 종료 시 복귀)
// ==========================================
void Monster::UpdateChase(float delta_time) {
    if (current_path_.empty() || path_index_ >= current_path_.size()) return;

    Vector3 next_waypoint = current_path_[path_index_];
    float speed = 2.0f;

    float dx = next_waypoint.x - position_.x;
    float dy = next_waypoint.y - position_.y;
    float distance = std::sqrt(dx * dx + dy * dy);

    // [추가] 유저가 내 공격 사거리(1.5f) 이내로 들어왔다면?
    if (distance <= attack_range_) {
        state_ = MonsterState::ATTACK;
        std::cout << "[Monster " << monster_id_ << "] ⚔️ 타겟 사거리 진입! 공격(ATTACK) 시작!\n";
        return;
    }

    if (distance < 0.1f) {
        path_index_++;
        // [수정] 유저를 쫓아왔는데 유저가 안 도망가고 가만히 있다면?
        // IDLE이나 RETURN으로 바꾸지 않고, CHASE 상태를 그대로 유지하며 계속 노려봅니다!
         

        //// 최종 목적지에 도달했을 때의 처리
        //if (path_index_ >= current_path_.size()) {
        //    // [TODO: 나중에는 여기서 유저가 아직 사거리 내에 있는지 확인하고 ATTACK 상태로 넘어갑니다.]

        //    // 임시 방어 코드: 현재 유저와 겹쳐서 도착한 상태라면, RETURN하지 않고 일단 멈춰서 노려봅니다.
        //    // (만약 GameServer가 유저가 도망간 것을 감지하고 다시 SetTarget을 호출해주면 경로가 갱신됩니다.)

        //    // 만약 유저가 완전히 멀어져서 타겟을 놓쳤다고 가정할 때만 RETURN 하도록 주석/로그 처리만 해둡니다.
        //    /*
        //    std::cout << "[Monster " << monster_id_ << "] 타겟을 놓쳤습니다. 제자리로 복귀(RETURN)합니다.\n";
        //    state_ = MonsterState::RETURN;
        //    target_last_pos_ = spawn_position_;
        //    CalculatePath();
        //    */

        //    // 지금은 목적지에 닿으면 그냥 IDLE로 얌전히 돌아가서 다음 틱에 다시 유저 좌표를 검사하게 합니다.
        //    state_ = MonsterState::IDLE;
        //}

        return;
    }

    position_.x += (dx / distance) * speed * delta_time;
    position_.y += (dy / distance) * speed * delta_time;
}

// ==========================================
// [수정] RETURN 상태 로직 (비상 탈출구 추가)
// ==========================================
void Monster::UpdateReturn(float delta_time) {
    // ★ [핵심] 경로가 텅 비었거나 다 왔다면, 무조건 강제로 IDLE 상태로 전환! 
    // (이렇게 해야 유저가 다시 다가왔을 때 정상적으로 어그로가 끌립니다)
    if (current_path_.empty() || path_index_ >= current_path_.size()) {
        position_ = spawn_position_;
        state_ = MonsterState::IDLE;
        std::cout << "[Monster " << monster_id_ << "] 고향으로 무사히 복귀 완료. 다시 경계(IDLE)를 시작합니다.\n";
        return;
    }

    Vector3 next_waypoint = current_path_[path_index_];
    float speed = 2.0f;

    float dx = next_waypoint.x - position_.x;
    float dy = next_waypoint.y - position_.y;
    float distance = std::sqrt(dx * dx + dy * dy);

    if (distance < 0.1f) {
        path_index_++;
        return;
    }

    position_.x += (dx / distance) * speed * delta_time;
    position_.y += (dy / distance) * speed * delta_time;
}

// UpdateAttack 로직 구현
void Monster::UpdateAttack(float delta_time) {
    float dx = target_last_pos_.x - position_.x;
    float dy = target_last_pos_.y - position_.y;
    float dist_to_target = std::sqrt(dx * dx + dy * dy);

    // 1. 유저가 사거리 밖으로 도망갔다면 다시 추적(CHASE)
    if (dist_to_target > attack_range_) {
        state_ = MonsterState::CHASE;
        std::cout << "[Monster " << monster_id_ << "] 🏃 타겟이 도망감. 다시 추적(CHASE) 재개!\n";
        CalculatePath();
        return;
    }

    // 2. 쿨타임(2초)마다 찰지게 때리기!
    attack_timer_ += delta_time;
    if (attack_timer_ >= attack_cooldown_) {
        attack_timer_ -= attack_cooldown_; // 쿨타임 초기화

        // 외부에 등록된 콜백(GameServer/MonsterManager)에게 타격 사실 알림
        if (on_attack_callback_) {
            on_attack_callback_(monster_id_, target_user_id_, attack_power_);
        }
    }
}

// ==========================================
// [수정] 길찾기 계산 로직 (멈칫거림 방지)
// ==========================================
void Monster::CalculatePath() {
    if (navmesh_) {
        current_path_ = navmesh_->FindPath(position_, target_last_pos_);
        path_index_ = 0;

        // ★ [핵심] Detour가 반환한 첫 번째 좌표가 '현재 내 위치'와 동일하다면 
        // 0번째를 건너뛰고 1번째(진짜 다음 목표)부터 이동하게 하여 멈칫거림 방지!
        if (current_path_.size() > 1) {
            float dx = current_path_[0].x - position_.x;
            float dy = current_path_[0].y - position_.y;
            if (std::sqrt(dx * dx + dy * dy) < 0.1f) {
                path_index_ = 1;
            }
        }
        //std::cout << "[Monster " << monster_id_ << "] 경로 계산 완료. 경유지 개수: " << current_path_.size() << "\n";

        // [테스트 로그] A* 알고리즘이 찾아낸 꺾임점(Waypoint) 출력
        //std::cout << "=================================================\n";
        //std::cout << "[Monster " << monster_id_ << "] 🧠 A* & Funnel 알고리즘 회피 경로 연산 완료!\n";
        //for (size_t i = path_index_; i < current_path_.size(); ++i) {
        //    std::cout << "    -> [경유지 " << i << "] X: " << current_path_[i].x << ", Y: " << current_path_[i].y << "\n";
        //}
        //std::cout << "=================================================\n";
    }
}