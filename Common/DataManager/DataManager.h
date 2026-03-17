#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>

#include "MonsterDataManager.h"

// ==========================================
// ★ 게임 내 모든 JSON/CSV 데이터를 관리하는 중앙 관리자
// ==========================================
class DataManager {
public:
    MonsterDataManager monsterData;

    DataManager() = default;
    ~DataManager() = default;

    DataManager(const DataManager&) = delete;
    DataManager& operator=(const DataManager&) = delete;

    // JsonData 폴더 내의 모든 파일을 로드하는 핵심 함수
    bool LoadAllData(const std::string& dataPath = "JsonData/");
};