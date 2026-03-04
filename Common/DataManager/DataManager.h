#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <iostream>

// ==========================================
// ★ 게임 내 모든 JSON/CSV 데이터를 관리하는 중앙 관리자
// ==========================================
class DataManager {
private:
    
    // 싱글톤 패턴
    DataManager() = default;
    ~DataManager() = default;

public:
    static DataManager& GetInstance() {
        static DataManager instance;
        return instance;
    }

    DataManager(const DataManager&) = delete;
    DataManager& operator=(const DataManager&) = delete;

    // JsonData 폴더 내의 모든 파일을 로드하는 핵심 함수
    bool LoadAllData(const std::string& dataPath = "JsonData/");
};