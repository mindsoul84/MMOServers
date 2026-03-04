#pragma once
#include <string>
#include <iostream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

class ConfigManager {
private:
    bool db_conn_ = false;
    std::string server_name_;
    std::string database_;

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

            // 2. 중첩된 속성(MSSQL_INFO) 읽기 (기본값도 설정해줍니다)
            server_name_ = pt.get<std::string>("MSSQL_INFO.ServerName", ".\\SQLEXPRESS");
            database_ = pt.get<std::string>("MSSQL_INFO.Database", "game_db");

            std::cout << "[ConfigManager] 환경 설정 로드 성공! (DB 연동: "
                << (db_conn_ ? "ON" : "OFF") << ")\n";
            if (db_conn_) {
                std::cout << "  -> Target DB: " << server_name_ << " / " << database_ << "\n";
            }
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

    // Getter 함수들
    const std::string& GetServerName() const { return server_name_; }
    const std::string& GetDatabase() const { return database_; }
};