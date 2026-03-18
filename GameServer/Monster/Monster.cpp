#include "Monster.h"
#include "../GameServer.h" // ★ [추가] GameContext 접근용
#include <iostream>
#include <boost/asio.hpp>

// ==========================================
// [삭제됨] 외부 전역 변수(extern g_ai_io_context, g_game_strand) 삭제 완료
// ==========================================

// 생성자 구현
Monster::Monster(uint64_t id, NavMesh* navmesh)
    : monster_id_(id), state_(MonsterState::IDLE), navmesh_(navmesh),
    target_user_id_(0), path_index_(0),
    hp_(100), max_hp_(100), attack_power_(15), // 기본 공격력 15,
    defense_power_(10), // 기본 방어력 10
    attack_range_(0.5f), attack_cooldown_(2.0f), attack_timer_(2.0f), // 첫 타격은 즉시 때리도록 타이머를 꽉 채워둠
    spawn_position_({ 0.0f, 0.0f, 0.0f }), target_last_pos_({ 0.0f, 0.0f, 0.0f })
{
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
    case MonsterState::RETURN:
        UpdateReturn(delta_time);
        break;
    case MonsterState::ATTACK:
        UpdateAttack(delta_time);
        break;
    }
}

// IDLE 상태 로직
void Monster::UpdateIdle() {
    // [로직] 주변(AOI)에 유저가 있는지 검색한다.
}

// 외부에서 타겟을 지정받았을 때의 처리
void Monster::SetTarget(uint64_t target_id, Vector3 target_pos) {
    target_user_id_ = target_id;
    target_last_pos_ = target_pos;

    // 이미 CHASE 상태가 아닐 때만 로그를 띄웁니다
    if (state_ != MonsterState::CHASE) {
        state_ = MonsterState::CHASE;
        std::cout << "[Monster " << monster_id_ << "] 🚨 유저(" << target_id << ") 발견! 추적(CHASE) 모드 가동!\n";
    }
    CalculatePath();
}

// 타겟이 1칸 범위 내에서 움직였을 때 목적지를 갱신하는 함수
void Monster::UpdateTargetPosition(Vector3 new_pos) {
    float dx = target_last_pos_.x - new_pos.x;
    float dy = target_last_pos_.y - new_pos.y;

    if (std::sqrt(dx * dx + dy * dy) > 0.1f) {
        target_last_pos_ = new_pos;
        CalculatePath();

        std::cout << "[Monster " << monster_id_ << "] 🏃 유저 이동(도착지 X:" << new_pos.x << ", Y:" << new_pos.y
            << ") -> 현재 몬스터 위치(X:" << position_.x << ", Y:" << position_.y << ")에서 추격 중!\n";
    }
}

// 유저가 너무 멀리 도망갔을 때 추적을 포기하는 함수
void Monster::GiveUpChase() {
    if (state_ == MonsterState::CHASE) {
        std::cout << "[Monster " << monster_id_ << "] 💨 거리가 멀어져 타겟을 놓쳤습니다. 제자리로 복귀(RETURN)합니다.\n";
        state_ = MonsterState::RETURN;
        target_last_pos_ = spawn_position_;
        CalculatePath();
    }
}

void Monster::GiveUpAttack() {
    state_ = MonsterState::RETURN;
    target_user_id_ = 0;

    std::cout << "[Monster " << monster_id_ << "] 🛑 타겟을 잃었습니다! 공격을 중지하고 고향으로 복귀(RETURN)합니다.\n";

    target_last_pos_ = spawn_position_;
    CalculatePath();
}

// CHASE 상태 로직 
void Monster::UpdateChase(float delta_time) {
    if (current_path_.empty() || path_index_ >= current_path_.size()) return;

    Vector3 next_waypoint = current_path_[path_index_];
    float speed = 2.0f;

    float dx = next_waypoint.x - position_.x;
    float dy = next_waypoint.y - position_.y;
    float distance = std::sqrt(dx * dx + dy * dy);

    if (distance <= attack_range_) {
        state_ = MonsterState::ATTACK;
        std::cout << "[Monster " << monster_id_ << "] ⚔️ 타겟 사거리 진입! 공격(ATTACK) 시작!\n";
        return;
    }

    if (distance < 0.1f) {
        path_index_++;
        return;
    }

    position_.x += (dx / distance) * speed * delta_time;
    position_.y += (dy / distance) * speed * delta_time;
}

// RETURN 상태 로직 
void Monster::UpdateReturn(float delta_time) {
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

    if (dist_to_target > attack_range_) {
        state_ = MonsterState::CHASE;
        std::cout << "[Monster " << monster_id_ << "] 🏃 타겟이 도망감. 다시 추적(CHASE) 재개!\n";
        CalculatePath();
        return;
    }

    attack_timer_ += delta_time;
    if (attack_timer_ >= attack_cooldown_) {
        attack_timer_ -= attack_cooldown_;

        if (on_attack_callback_) {
            on_attack_callback_(monster_id_, target_user_id_, attack_power_);
        }
    }
}

// ==========================================
// ★ [수정] 길찾기 연산 비동기 처리 (GameContext 위임)
// ==========================================
void Monster::CalculatePath() {
    if (!navmesh_) return;

    auto self = shared_from_this();
    Vector3 start_pos = position_;
    Vector3 end_pos = target_last_pos_;
    NavMesh* current_nav = navmesh_;

    // 1. 메인 스레드(Strand) -> AI 스레드 풀(GameContext)에 무거운 작업 위임
    boost::asio::post(GameContext::Get().ai_io_context, [self, start_pos, end_pos, current_nav]() {

        std::vector<Vector3> result_waypoints = current_nav->FindPath(start_pos, end_pos);

        // 2. 연산이 끝나면, 메인 게임 Strand(GameContext)로 결과 보고
        boost::asio::post(GameContext::Get().io_context, [self, result_waypoints]() {

            if (self->state_ == MonsterState::DEAD) return;

            self->current_path_ = result_waypoints;
            self->path_index_ = 0;

            if (self->current_path_.size() > 1) {
                float dx = self->current_path_[0].x - self->position_.x;
                float dy = self->current_path_[0].y - self->position_.y;
                if (std::sqrt(dx * dx + dy * dy) < 0.1f) {
                    self->path_index_ = 1;
                }
            }
            });
        });
}