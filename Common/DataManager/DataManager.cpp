#include "DataManager.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

bool DataManager::LoadAllData(const std::string& dataPath) {
    bool isSuccess = true;

    // 1. 몬스터 데이터 로드 지시
    if (!monsterData.LoadMonsterData(dataPath)) isSuccess = false;
    
    if (isSuccess) {
        std::cout << "[DataManager] 모든 기획 json 데이터 세팅 완료 성공.\n";
    }

    return isSuccess;
}