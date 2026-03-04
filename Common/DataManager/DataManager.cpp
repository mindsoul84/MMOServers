#include "DataManager.h"
#include "MonsterDataManager.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

bool DataManager::LoadAllData(const std::string& dataPath) {
    bool isSuccess = true;

    // 1. 몬스터 데이터 로드 지시
    if (!MonsterDataManager::GetInstance().LoadMonsterData(dataPath)) {
        isSuccess = false;
    }
    
    if (isSuccess) {
        std::cout << "[DataManager] ✅ 모든 기획 json 데이터 세팅이 성공적으로 완료되었습니다.\n";
    }

    return isSuccess;
}