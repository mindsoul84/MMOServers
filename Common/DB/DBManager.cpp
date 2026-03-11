#include "DBManager.h"
#include "..\ConfigManager.h"
#include <iostream>

// ★ 전역 thread_local 변수 실제 메모리 할당 (정의)
thread_local DBManager* t_dbManager = nullptr;

bool DBManager::Connect() {
    SQLRETURN retcode;

    // 1. 환경 핸들 할당
    retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv_);
    if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) return false;

    // 2. ODBC 버전 설정 
    // ★ 수정: (SQLPOINTER)SQL_OV_ODBC3 대신 reinterpret_cast를 사용하여 x64 포인터 경고 해결
    retcode = SQLSetEnvAttr(henv_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_OV_ODBC3)), 0);
    if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) return false;

    // 3. 연결 핸들 할당
    retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv_, &hdbc_);
    if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) return false;

    // ==========================================================
    // ConfigManager에서 값을 가져와 동적으로 설정
    // ==========================================================
    std::string serverName = ConfigManager::GetInstance().GetServerName();
    std::string database = ConfigManager::GetInstance().GetDatabase();

    std::string connStr = "Driver={SQL Server};Server=" + serverName + ";Database=" + database + ";Trusted_Connection=yes;";

    SQLCHAR outConnStr[1024];
    SQLSMALLINT outConnStrLen;

    // std::string을 사용하므로 SQLDriverConnectA (ANSI 버전) 호출
    retcode = SQLDriverConnectA(
        hdbc_,
        NULL,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(connStr.c_str())),
        SQL_NTS,
        outConnStr,
        1024,
        &outConnStrLen,
        SQL_DRIVER_NOPROMPT
    );

    if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
        std::cout << "[DBManager] MSSQL (.\\SQLEXPRESS - game_db) 연결 성공! (Windows 인증)\n";
        return true;
    }
    else {
        std::cerr << "[DBManager] DB 연결 실패!\n";
        PrintError(SQL_HANDLE_DBC, hdbc_);
        return false;
    }
}

void DBManager::Disconnect() {
    if (hdbc_ != SQL_NULL_HDBC) {
        SQLDisconnect(hdbc_);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc_);
        hdbc_ = SQL_NULL_HDBC;
        std::cout << "[DBManager] DB 연결 해제됨.\n";
    }
    if (henv_ != SQL_NULL_HENV) {
        SQLFreeHandle(SQL_HANDLE_ENV, henv_);
        henv_ = SQL_NULL_HENV;
    }
}

void DBManager::PrintError(SQLSMALLINT handleType, SQLHANDLE handle) {
    SQLWCHAR sqlState[6], message[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER nativeError;
    SQLSMALLINT textLength;

    SQLGetDiagRecW(handleType, handle, 1, sqlState, &nativeError, message, SQL_MAX_MESSAGE_LENGTH, &textLength);
    std::wcerr << L"SQL Error State: " << sqlState << L", Message: " << message << L"\n";
}

// ★ [추가] 로그인 검증 및 자동 가입 로직
LoginResult DBManager::ProcessLogin(const std::string& id, const std::string& pw, int input_type) {
    if (!hdbc_) return LoginResult::DB_ERROR; // 연결 상태 확인

    SQLHSTMT stmt;
    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
        return LoginResult::DB_ERROR;
    }

    // 1. 계정 존재 여부 및 비밀번호 조회
    std::string query = "SELECT password FROM Accounts WHERE account_id = '" + id + "'";
    SQLExecDirectA(stmt, (SQLCHAR*)query.c_str(), SQL_NTS);

    SQLCHAR db_pw[256] = { 0 };
    SQLLEN cb_pw = 0;

    SQLRETURN ret = SQLFetch(stmt);
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        // [계정이 존재함] -> 비밀번호 대조
        SQLGetData(stmt, 1, SQL_C_CHAR, db_pw, sizeof(db_pw), &cb_pw);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);

        std::string stored_pw(reinterpret_cast<char*>(db_pw));
        if (stored_pw == pw) {
            return LoginResult::SUCCESS; // 비밀번호 일치
        }
        else {
            return LoginResult::WRONG_PASSWORD; // 비밀번호 불일치
        }
    }
    else {
        // 2. [계정이 없음] -> 자동 회원가입(INSERT) 처리
        SQLFreeHandle(SQL_HANDLE_STMT, stmt); // 기존 핸들러 닫기
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt); // 새 핸들러 열기

        // input_type을 std::to_string()으로 감싸서 문자열로 변환해 줍니다!
        std::string insert_query = "INSERT INTO Accounts (account_id, password, input) VALUES ('" + id + "', '" + pw + "', " + std::to_string(input_type) + ")";
        ret = SQLExecDirectA(stmt, (SQLCHAR*)insert_query.c_str(), SQL_NTS);

        SQLFreeHandle(SQL_HANDLE_STMT, stmt);

        if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
            return LoginResult::NEW_REGISTERED; // 가입 완료
        }
        else {
            return LoginResult::DB_ERROR; // DB 에러
        }
    }
}