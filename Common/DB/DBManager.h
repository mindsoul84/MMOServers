#pragma once

// 보안 관련 경고(CRT_SECURE) 방지
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <iostream>
#include <string>
#include <memory>
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

    // 복사/이동 금지 (ODBC 핸들은 복사 불가)
    DBManager(const DBManager&) = delete;
    DBManager& operator=(const DBManager&) = delete;

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
// raw 포인터 → unique_ptr 로 변경
//   기존: thread_local DBManager* t_dbManager = nullptr;
//   수정: 예외 발생 시에도 안전하게 자원 해제 보장
// =========================================================
extern thread_local std::unique_ptr<DBManager> t_dbManager;
