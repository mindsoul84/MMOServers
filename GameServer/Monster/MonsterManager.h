#pragma once

// 몬스터 초기 스폰을 담당하는 함수
void InitMonsters();

// AI 몬스터들의 메인 게임 루프(심장)를 백그라운드 스레드로 가동하는 함수
void StartAITickThread();