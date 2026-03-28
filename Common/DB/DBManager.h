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

// 로그인 결과 Enum
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

    // ==========================================
    // [수정] DB 스키마 변경 반영 (account_id -> account_name, account_uid 추가)
    //
    // 변경 전: ProcessLogin(id, pw, input_type)
    //   -> Accounts.account_id 컬럼으로 조회/삽입
    //
    // 변경 후: ProcessLogin(id, pw, input_type, out_account_uid)
    //   -> Accounts.account_name 컬럼으로 조회/삽입
    //   -> 로그인 성공 또는 신규 가입 시 account_uid를 out_account_uid에 반환
    //   -> 기존 호출부 호환: out_account_uid 기본값 nullptr
    // ==========================================
    LoginResult ProcessLogin(const std::string& id, const std::string& pw, int input_type, int64_t* out_account_uid = nullptr);
};

// =========================================================
// raw 포인터 → unique_ptr 로 변경
//   기존: thread_local DBManager* t_dbManager = nullptr;
//   수정: 예외 발생 시에도 안전하게 자원 해제 보장
// =========================================================
extern thread_local std::unique_ptr<DBManager> t_dbManager;
