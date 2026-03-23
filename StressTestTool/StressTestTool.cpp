#include <iostream>
#include <boost/asio.hpp>
#include <thread>
#include <vector>
#include <windows.h>
#include "../StressTestTool/Manager/StressManager.h"
#include "../Common/ConfigManager.h"

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // =========================================================
    // ★ [중복 실행 방지] 고유한 이름의 Named Mutex 생성
    // (이름은 다른 프로그램과 겹치지 않게 고유한 문자열로 지정합니다)
    // =========================================================
    HANDLE hMutex = CreateMutex(NULL, FALSE, L"Global\\StressTestTool_Unique_Mutex_Lock");

    // 방금 만든 뮤텍스가 이미 존재한다면? -> 다른 누군가(먼저 실행된 툴)가 쥐고 있다는 뜻!
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "🚨 [Error] StressTestTool이 이미 실행 중입니다. 창을 닫습니다.\n";
        CloseHandle(hMutex); // 핸들 정리
        return 1; // 여기서 프로그램을 즉시 종료합니다.
    }
    // =========================================================

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

    // 프로그램이 정상적으로 완전히 끝날 때 자물쇠를 반납합니다.
    // (사실 프로그램이 강제 종료되어도 OS가 알아서 수거해 주므로 매우 안전합니다.)
    if (hMutex) {
        CloseHandle(hMutex);
    }

    return 0;
}