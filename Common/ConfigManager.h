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

    // 각 서버별 설정 Default 변수
    short dummy_client_login_port_;

    short game_server_port_;
    short game_world_conn_port_;
    int game_max_thread_count_;
    int game_ai_thread_count_;

    short gateway_server_port_;
    short gateway_game_conn_port_;
    int gateway_max_thread_count_;

    short login_server_port_;
    short login_world_conn_port_;
    int login_max_thread_count_;
    int login_db_thread_count_;

    short world_server_port_;

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

            // JSON 데이터 파싱 (기본값 세팅 포함)
            dummy_client_login_port_ = pt.get<short>("dummy_client_info.login_server_port");

            game_server_port_ = pt.get<short>("game_server_info.game_server_port");
            game_world_conn_port_ = pt.get<short>("game_server_info.world_conn_port");
            game_max_thread_count_ = pt.get<int>("game_server_info.max_thread_count");
            game_ai_thread_count_ = pt.get<int>("game_server_info.ai_thread_count");

            gateway_server_port_ = pt.get<short>("gateway_server_info.gateway_server_port");
            gateway_game_conn_port_ = pt.get<short>("gateway_server_info.game_conn_port");
            gateway_max_thread_count_ = pt.get<int>("gateway_server_info.max_thread_count");

            login_server_port_ = pt.get<short>("login_server_info.login_server_port");
            login_world_conn_port_ = pt.get<short>("login_server_info.world_conn_port");
            login_max_thread_count_ = pt.get<int>("login_server_info.max_thread_count");
            login_db_thread_count_ = pt.get<int>("login_server_info.db_thread_count");

            world_server_port_ = pt.get<short>("world_server_info.world_server_port");

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

    short GetDummyClientLoginPort() const { return dummy_client_login_port_; }

    short GetGameServerPort() const { return game_server_port_; }
    short GetGameWorldConnPort() const { return game_world_conn_port_; }
    int GetGameMaxThreadCount() const { return game_max_thread_count_; }
    int GetGameAiThreadCount() const { return game_ai_thread_count_; }

    short GetGatewayServerPort() const { return gateway_server_port_; }
    short GetGatewayGameConnPort() const { return gateway_game_conn_port_; }
    int GetGatewayMaxThreadCount() const { return gateway_max_thread_count_; }

    short GetLoginServerPort() const { return login_server_port_; }
    short GetLoginWorldConnPort() const { return login_world_conn_port_; }
    int GetLoginMaxThreadCount() const { return login_max_thread_count_; }
    int GetLoginDbThreadCount() const { return login_db_thread_count_; }

    short GetWorldServerPort() const { return world_server_port_; }
};