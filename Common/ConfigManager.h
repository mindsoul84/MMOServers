#pragma once
#include <string>
#include <iostream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

// ==========================================
// ★ [수정 1] 싱글톤 남용 개선: 의존성 주입(DI) 지원
//
// 변경 전: private 생성자 + static Get() → 테스트 불가, 의존성 주입 불가능
// 변경 후: public 생성자 + SetTestInstance() → 테스트에서 목(mock) 인스턴스 주입 가능
//
// [테스트 코드 사용 예]
//   ConfigManager testCfg;
//   testCfg.SetGameServerPort(12345);
//   ConfigManager::SetTestInstance(&testCfg);
//   // ... 테스트 수행 ...
//   ConfigManager::SetTestInstance(nullptr); // 원래 싱글톤으로 복원
// ==========================================
class ConfigManager {
private:
    bool db_conn_ = false;
    std::string server_name_;
    std::string database_;

    short dummy_client_login_port_    = 0;
    short game_server_port_           = 0;
    short game_world_conn_port_       = 0;
    int   game_max_thread_count_      = 0;
    int   game_ai_thread_count_       = 0;
    short gateway_server_port_        = 0;
    short gateway_game_conn_port_     = 0;
    int   gateway_max_thread_count_   = 0;
    short login_server_port_          = 0;
    short login_world_conn_port_      = 0;
    int   login_max_thread_count_     = 0;
    int   login_db_thread_count_      = 0;
    short world_server_port_          = 0;
    int   stress_target_connections_  = 0;
    int   stress_spawn_rate_          = 0;
    int   stress_worker_threads_      = 0;
    std::string stress_login_server_ip_;
    short stress_login_server_port_   = 0;

    // ★ [수정] inline 정의 사용 (C++17) → 별도 .cpp 파일 불필요
    // 각 프로젝트마다 ConfigManager.cpp를 추가할 필요 없이 헤더만으로 링크 완결
    inline static ConfigManager* s_test_instance_ = nullptr;

public:
    // ★ [수정] private → public 생성자: 테스트에서 직접 인스턴스 생성 가능
    ConfigManager() = default;

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // ★ [수정] 테스트 인스턴스가 주입된 경우 그것을 반환, 아니면 기존 정적 싱글톤 반환
    static ConfigManager& GetInstance() {
        if (s_test_instance_) return *s_test_instance_;
        static ConfigManager instance;
        return instance;
    }

    // ★ [추가] 테스트 전용 주입 메서드. nullptr 전달 시 프로덕션 싱글톤으로 복원
    static void SetTestInstance(ConfigManager* instance) noexcept {
        s_test_instance_ = instance;
    }

    bool LoadConfig(const std::string& filePath = "config.json") {
        try {
            boost::property_tree::ptree pt;
            boost::property_tree::read_json(filePath, pt);

            db_conn_     = pt.get<bool>("db_conn", false);
            server_name_ = pt.get<std::string>("MSSQL_INFO.ServerName", ".\\SQLEXPRESS");
            database_    = pt.get<std::string>("MSSQL_INFO.Database", "game_db");

            dummy_client_login_port_ = pt.get<short>("dummy_client_info.login_server_port");

            game_server_port_        = pt.get<short>("game_server_info.game_server_port");
            game_world_conn_port_    = pt.get<short>("game_server_info.world_conn_port");
            game_max_thread_count_   = pt.get<int>("game_server_info.max_thread_count");
            game_ai_thread_count_    = pt.get<int>("game_server_info.ai_thread_count");

            gateway_server_port_     = pt.get<short>("gateway_server_info.gateway_server_port");
            gateway_game_conn_port_  = pt.get<short>("gateway_server_info.game_conn_port");
            gateway_max_thread_count_ = pt.get<int>("gateway_server_info.max_thread_count");

            login_server_port_       = pt.get<short>("login_server_info.login_server_port");
            login_world_conn_port_   = pt.get<short>("login_server_info.world_conn_port");
            login_max_thread_count_  = pt.get<int>("login_server_info.max_thread_count");
            login_db_thread_count_   = pt.get<int>("login_server_info.db_thread_count");

            world_server_port_ = pt.get<short>("world_server_info.world_server_port");

            stress_target_connections_ = pt.get<int>("stress_test_tool_info.target_connections");
            stress_spawn_rate_         = pt.get<int>("stress_test_tool_info.spawn_rate");
            stress_worker_threads_     = pt.get<int>("stress_test_tool_info.worker_threads");
            stress_login_server_ip_    = pt.get<std::string>("stress_test_tool_info.login_server_ip");
            stress_login_server_port_  = pt.get<short>("stress_test_tool_info.login_server_port");

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

    bool UseDB() const { return db_conn_; }

    const std::string& GetServerName()  const { return server_name_; }
    const std::string& GetDatabase()    const { return database_; }

    short GetDummyClientLoginPort()     const { return dummy_client_login_port_; }
    short GetGameServerPort()           const { return game_server_port_; }
    short GetGameWorldConnPort()        const { return game_world_conn_port_; }
    int   GetGameMaxThreadCount()       const { return game_max_thread_count_; }
    int   GetGameAiThreadCount()        const { return game_ai_thread_count_; }
    short GetGatewayServerPort()        const { return gateway_server_port_; }
    short GetGatewayGameConnPort()      const { return gateway_game_conn_port_; }
    int   GetGatewayMaxThreadCount()    const { return gateway_max_thread_count_; }
    short GetLoginServerPort()          const { return login_server_port_; }
    short GetLoginWorldConnPort()       const { return login_world_conn_port_; }
    int   GetLoginMaxThreadCount()      const { return login_max_thread_count_; }
    int   GetLoginDbThreadCount()       const { return login_db_thread_count_; }
    short GetWorldServerPort()          const { return world_server_port_; }
    int   GetStressTargetConnections()  const { return stress_target_connections_; }
    int   GetStressSpawnRate()          const { return stress_spawn_rate_; }
    int   GetStressWorkerThreads()      const { return stress_worker_threads_; }
    const std::string& GetStressLoginServerIp() const { return stress_login_server_ip_; }
    short GetStressLoginServerPort()    const { return stress_login_server_port_; }

    // ★ [추가] 테스트 편의 Setter (프로덕션에서는 LoadConfig() 사용)
    void SetUseDB(bool use)                 { db_conn_ = use; }
    void SetGameServerPort(short port)      { game_server_port_ = port; }
    void SetLoginServerPort(short port)     { login_server_port_ = port; }
    void SetGatewayServerPort(short port)   { gateway_server_port_ = port; }
    void SetLoginDbThreadCount(int cnt)     { login_db_thread_count_ = cnt; }
    void SetGameAiThreadCount(int cnt)      { game_ai_thread_count_ = cnt; }
    void SetGameMaxThreadCount(int cnt)     { game_max_thread_count_ = cnt; }
    void SetLoginMaxThreadCount(int cnt)    { login_max_thread_count_ = cnt; }
    void SetGatewayMaxThreadCount(int cnt)  { gateway_max_thread_count_ = cnt; }
};
