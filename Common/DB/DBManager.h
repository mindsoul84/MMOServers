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

class DBManager {
private:
    SQLHENV henv_ = SQL_NULL_HENV; // 환경 핸들
    SQLHDBC hdbc_ = SQL_NULL_HDBC; // 연결 핸들

    // 싱글톤 패턴
    DBManager() = default;
    ~DBManager() { Disconnect(); }

public:
    static DBManager& GetInstance() {
        static DBManager instance;
        return instance;
    }

    DBManager(const DBManager&) = delete;
    DBManager& operator=(const DBManager&) = delete;

    // DB 연결 (config.json 세팅에 따라 호출됨)
    bool Connect();

    // DB 연결 해제
    void Disconnect();

    // 에러 출력 헬퍼 함수
    void PrintError(SQLSMALLINT handleType, SQLHANDLE handle);
};