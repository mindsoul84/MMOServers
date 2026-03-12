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

// ★ [보안 수정] SQL Injection 방지 - Prepared Statement (SQLBindParameter) 사용
// ❌ 기존 방식: "SELECT ... WHERE id = '" + id + "'"  → SQL Injection 취약
// ✅ 수정 방식: SQLPrepare + SQLBindParameter → 입력값을 코드가 아닌 '데이터'로만 처리
LoginResult DBManager::ProcessLogin(const std::string& id, const std::string& pw, int input_type) {
    if (!hdbc_) return LoginResult::DB_ERROR;

    // =========================================================
    // [입력값 길이 검증] 바인딩 전에 반드시 먼저 실시
    // =========================================================
    if (id.empty() || id.size() > 50 || pw.empty() || pw.size() > 100) {
        std::cerr << "[DBManager] 잘못된 입력값 길이 (id:" << id.size() << ", pw:" << pw.size() << ")\n";
        return LoginResult::DB_ERROR;
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
        return LoginResult::DB_ERROR;
    }

    // =========================================================
    // STEP 1. 계정 존재 여부 및 비밀번호 조회 (Prepared Statement)
    // =========================================================
    // '?' 플레이스홀더를 사용하여 쿼리 구조와 데이터를 완전히 분리합니다.
    const char* select_sql = "SELECT password FROM Accounts WHERE account_id = ?";
    SQLRETURN ret = SQLPrepareA(stmt, (SQLCHAR*)select_sql, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        std::cerr << "[DBManager] SELECT Prepare 실패\n";
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    // id 값을 1번 파라미터('?')에 안전하게 바인딩
    SQLLEN id_len = SQL_NTS;
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        50, 0, (SQLPOINTER)id.c_str(), id.size(), &id_len);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        std::cerr << "[DBManager] SELECT BindParameter 실패\n";
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    ret = SQLExecute(stmt);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        std::cerr << "[DBManager] SELECT Execute 실패\n";
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    SQLCHAR db_pw[128] = { 0 };
    SQLLEN cb_pw = 0;
    ret = SQLFetch(stmt);

    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        // [계정 존재] → 비밀번호 대조
        SQLGetData(stmt, 1, SQL_C_CHAR, db_pw, sizeof(db_pw), &cb_pw);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);

        std::string stored_pw(reinterpret_cast<char*>(db_pw));
        return (stored_pw == pw) ? LoginResult::SUCCESS : LoginResult::WRONG_PASSWORD;
    }

    // =========================================================
    // STEP 2. 계정 없음 → 자동 회원가입 (INSERT Prepared Statement)
    // =========================================================
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    stmt = SQL_NULL_HSTMT;

    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
        return LoginResult::DB_ERROR;
    }

    const char* insert_sql = "INSERT INTO Accounts (account_id, password, input) VALUES (?, ?, ?)";
    ret = SQLPrepareA(stmt, (SQLCHAR*)insert_sql, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        std::cerr << "[DBManager] INSERT Prepare 실패\n";
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    // 1번 파라미터: account_id
    SQLLEN pw_len = SQL_NTS;
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        50, 0, (SQLPOINTER)id.c_str(), id.size(), &id_len);

    // 2번 파라미터: password
    SQLLEN pw_bind_len = SQL_NTS;
    ret = SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        100, 0, (SQLPOINTER)pw.c_str(), pw.size(), &pw_bind_len);

    // 3번 파라미터: input (정수형)
    SQLLEN input_len = 0;
    ret = SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
        0, 0, (SQLPOINTER)&input_type, sizeof(int), &input_len);

    ret = SQLExecute(stmt);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        return LoginResult::NEW_REGISTERED;
    }

    std::cerr << "[DBManager] INSERT Execute 실패\n";
    return LoginResult::DB_ERROR;
}