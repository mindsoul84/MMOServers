#include <iostream>
#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <windows.h>
#include "StressManager.h"
#include "../Common/ConfigManager.h"

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // Config 로드
    if (!ConfigManager::GetInstance().LoadConfig("config.json")) {
        std::cerr << "🚨 config.json 설정 파일 로드에 실패하여 종료합니다.\n";
        system("pause");
        return -1;
    }

    // ==========================================
    // ⚙️ 부하 테스트 설정값
    // ==========================================
    int TARGET_CONNECTIONS = ConfigManager::GetInstance().GetStressTargetConnections();  // 목표 동시 접속자 수
    int SPAWN_RATE = ConfigManager::GetInstance().GetStressSpawnRate();                  // 1초당 접속시킬 봇의 수 (너무 높으면 서버/OS에서 튕김)
    int WORKER_THREADS = ConfigManager::GetInstance().GetStressWorkerThreads();         // 클라이언트 네트워크 처리를 담당할 스레드 수
    // ==========================================

    boost::asio::io_context io_context;

    StressManager manager(io_context, TARGET_CONNECTIONS, SPAWN_RATE);
    manager.StartStressTest();

    // 멀티스레드 워커 풀 가동
    std::vector<std::thread> threads;
    for (int i = 0; i < WORKER_THREADS; ++i) {
        threads.emplace_back([&io_context]() {
            io_context.run();
            });
    }

    // 메인 스레드 대기
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    return 0;
}