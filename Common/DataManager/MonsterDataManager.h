#pragma once
#include <vector>
#include <string>
#include <cstdint>

// 몬스터 스폰 데이터 구조체
struct MonsterSpawnData {
    uint64_t mon_id;
    float x;
    float y;
};

class MonsterDataManager {
private:
    std::vector<MonsterSpawnData> monsterSpawnList_;

    // 싱글톤 패턴
    MonsterDataManager() = default;

public:
    static MonsterDataManager& GetInstance() {
        static MonsterDataManager instance;
        return instance;
    }

    MonsterDataManager(const MonsterDataManager&) = delete;
    MonsterDataManager& operator=(const MonsterDataManager&) = delete;

    // 몬스터 전용 JSON 로드 함수
    bool LoadMonsterData(const std::string& dataPath);

    // 데이터 제공 Getter
    const std::vector<MonsterSpawnData>& GetMonsterSpawnList() const {
        return monsterSpawnList_;
    }
};