#pragma once

// 보안 관련 경고(CRT_SECURE) 방지
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <iostream>
#include <string>
#include <windows.h>
#include <sql.h>
#include <sqlext.h>

// 클래스 외부(상단)에 로그인 결과 Enum 추가
enum class LoginResult {
    SUCCESS,
    NEW_REGISTERED,
    WRONG_PASSWORD,
    DB_ERROR
};

class DBManager {
private:
    SQLHENV henv_ = SQL_NULL_HENV; // 환경 핸들
    SQLHDBC hdbc_ = SQL_NULL_HDBC; // 연결 핸들    

public:
    // ★ 1. DBManager ODBC Thread-Local DB Connection Pool 도입: 싱글톤 삭제, 기본 생성자 public 이동

    DBManager() = default;
    ~DBManager() { Disconnect(); }

    // DB 연결 (config.json 세팅에 따라 호출됨)
    bool Connect();

    // DB 연결 해제
    void Disconnect();

    // 에러 출력 헬퍼 함수
    void PrintError(SQLSMALLINT handleType, SQLHANDLE handle);

    // DBManager 클래스 내부에 함수 원형 추가
    LoginResult ProcessLogin(const std::string& id, const std::string& pw, int input_type);
};

// =========================================================
// ★ 2. [핵심] 이 스레드만의 전용 DB 매니저를 가리키는 포인터 선언!
// =========================================================
extern thread_local DBManager* t_dbManager;