#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>

#include "MonsterDataManager.h"

// ==========================================
// 게임 내 모든 JSON 데이터 관리
// ==========================================
class DataManager {
public:    

    DataManager() = default;
    ~DataManager() = default;

    DataManager(const DataManager&) = delete;
    DataManager& operator=(const DataManager&) = delete;

    // JsonData 폴더 내의 모든 파일을 로드하는 핵심 함수
    bool LoadAllData(const std::string& dataPath = "JsonData/");

    const MonsterDataManager& GetMonsterData() const { return monsterData; }

private:
    MonsterDataManager monsterData;
};