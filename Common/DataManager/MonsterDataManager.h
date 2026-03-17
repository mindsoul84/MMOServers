#pragma once
#include <vector>
#include <string>
#include <cstdint>

// 몬스터 스폰 데이터 구조체
struct MonsterSpawnData {
    uint64_t mon_id;
    float x;
    float y;
    int hp;             // 몬스터 체력
    int respawn_sec;    // 리스폰 시간(초)
};

class MonsterDataManager {
private:
    std::vector<MonsterSpawnData> monsterSpawnList_;    

public:

    MonsterDataManager() = default;
    ~MonsterDataManager() = default;
    
    MonsterDataManager(const MonsterDataManager&) = delete;
    MonsterDataManager& operator=(const MonsterDataManager&) = delete;

    // 몬스터 전용 JSON 로드 함수
    bool LoadMonsterData(const std::string& dataPath);

    // 데이터 제공 Getter
    const std::vector<MonsterSpawnData>& GetMonsterSpawnList() const {
        return monsterSpawnList_;
    }
};