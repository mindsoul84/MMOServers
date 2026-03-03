#pragma once
#include <string>
#include <iostream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

class ConfigManager {
private:
    bool db_conn_ = false;

    // 싱글톤 패턴
    ConfigManager() = default;

public:
    static ConfigManager& GetInstance() {
        static ConfigManager instance;
        return instance;
    }

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // JSON 설정 파일 로드
    bool LoadConfig(const std::string& filePath = "config.json") {
        try {
            boost::property_tree::ptree pt;
            // JSON 파일을 읽어서 Property Tree에 담습니다.
            boost::property_tree::read_json(filePath, pt);

            // "db_conn" 키의 값을 읽어옵니다. (기본값은 false로 세팅)
            db_conn_ = pt.get<bool>("db_conn", false);

            std::cout << "[ConfigManager] ⚙️ 환경 설정 로드 성공! (DB 연동: "
                << (db_conn_ ? "ON" : "OFF") << ")\n";
            return true;
        }
        catch (const std::exception& e) {
            std::cerr << "[ConfigManager] 🚨 설정 파일(" << filePath << ") 읽기 실패: " << e.what() << "\n";
            std::cerr << "[ConfigManager] 기본 설정(DB 연동 OFF)으로 진행합니다.\n";
            return false;
        }
    }

    // DB 연동 여부 반환
    bool UseDB() const { return db_conn_; }
};