#include "Monster.h"
#include <iostream>
#include <boost/asio.hpp>   // ★ [비동기 post 사용 위함

// ★ GameServer.cpp에 있는 전역 변수(스레드 풀과 메인 대기열)를 가져옵니다.
extern boost::asio::io_context g_ai_io_context;
extern boost::asio::io_context::strand g_game_strand;

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
// [수정] 길찾기 연산 비동기 처리 (AI 스레드 풀 위임)
// ==========================================
void Monster::CalculatePath() {
    if (!navmesh_) return;

    // 길찾기 연산 중에 몬스터가 죽어서 삭제되는 것을 막기 위해 shared_ptr 복사본(self)을 만듭니다.
    auto self = shared_from_this();

    // 현재 내 위치와 목표 위치를 람다 캡처를 위해 지역 변수로 복사합니다.
    Vector3 start_pos = position_;
    Vector3 end_pos = target_last_pos_;
    NavMesh* current_nav = navmesh_;

    // 3. 메인 스레드(Strand) -> AI 스레드 풀에 무거운 길찾기 작업(Job)을 던집니다.
    boost::asio::post(g_ai_io_context, [self, start_pos, end_pos, current_nav]() {

        // ----------------------------------------------------
        // [여기는 AI 스레드 풀 내부입니다]
        // ----------------------------------------------------
        // 실제 연산은 여기서 일어납니다. 메인 네트워크 로직은 블로킹되지 않습니다!
        std::vector<Vector3> result_waypoints = current_nav->FindPath(start_pos, end_pos);

        // 4. 연산이 끝나면, 그 결과를 다시 메인 게임 Strand로 콜백(보고) 합니다.
        boost::asio::post(g_game_strand, [self, result_waypoints]() {

            // ----------------------------------------------------
            // [여기는 다시 메인 게임 Strand 내부입니다]
            // ----------------------------------------------------

            // 콜백이 돌아왔는데 그 사이에 몬스터가 맞아 죽었다면 경로를 무시합니다.
            if (self->state_ == MonsterState::DEAD) return;

            // 계산된 새 경로를 적용합니다.
            self->current_path_ = result_waypoints;
            self->path_index_ = 0;

            // 멈칫거림 방지 로직 
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