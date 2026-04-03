#include "MonsterDataManager.h"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <iostream>

bool MonsterDataManager::LoadMonsterData(const std::string& dataPath) {
    try {
        std::string filePath = dataPath + "MONSTER.json";
        boost::property_tree::ptree pt;
        boost::property_tree::read_json(filePath, pt);

        monsterSpawnList_.clear();

        for (const auto& item : pt) {
            MonsterSpawnData data;
            data.mon_id = item.second.get<uint64_t>("mon_id");
            data.x = item.second.get<float>("mon_pos.x");
            data.y = item.second.get<float>("mon_pos.y");
            
            //   체력과 리스폰 시간 파싱 (안전하게 기본값 세팅)
            data.hp = item.second.get<int>("mon_hp", 100);
            data.respawn_sec = item.second.get<int>("mon_respawn_sec", 60);

            monsterSpawnList_.push_back(data);
        }
        std::cout << "[MonsterDataManager] MONSTER 데이터 로드 완료! (총 " << monsterSpawnList_.size() << "종)\n";
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[MonsterDataManager] MONSTER.json 로드 실패: " << e.what() << "\n";
        return false;
    }
}