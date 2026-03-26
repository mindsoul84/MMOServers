#include "DBManager.h"
#include "..\ConfigManager.h"
#include "..\Utils\CryptoUtils.h"
#include "..\Utils\Logger.h"
#include <iostream>
#include <memory>

// raw 포인터 → unique_ptr 로 변경
thread_local std::unique_ptr<DBManager> t_dbManager = nullptr;

bool DBManager::Connect() {
    SQLRETURN retcode;

    // 1. 환경 핸들 할당
    retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv_);
    if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) return false;

    // 2. ODBC 버전 설정 
    retcode = SQLSetEnvAttr(henv_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_OV_ODBC3)), 0);
    if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) return false;

    // 3. 연결 핸들 할당
    retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv_, &hdbc_);
    if (retcode != SQL_SUCCESS && retcode != SQL_SUCCESS_WITH_INFO) return false;

    std::string serverName = ConfigManager::GetInstance().GetServerName();
    std::string database = ConfigManager::GetInstance().GetDatabase();

    std::string connStr = "Driver={SQL Server};Server=" + serverName + ";Database=" + database + ";Trusted_Connection=yes;";

    SQLCHAR outConnStr[1024];
    SQLSMALLINT outConnStrLen;

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
        LOG_INFO("DBManager", "MSSQL (.\\" "SQLEXPRESS - game_db) 연결 성공! (Windows 인증)");
        return true;
    }
    else {
        LOG_ERROR("DBManager", "DB 연결 실패!");
        PrintError(SQL_HANDLE_DBC, hdbc_);
        return false;
    }
}

void DBManager::Disconnect() {
    if (hdbc_ != SQL_NULL_HDBC) {
        SQLDisconnect(hdbc_);
        SQLFreeHandle(SQL_HANDLE_DBC, hdbc_);
        hdbc_ = SQL_NULL_HDBC;
        LOG_INFO("DBManager", "DB 연결 해제됨.");
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

// ==========================================
// 모든 ODBC 호출의 리턴값 검증 강화
//
// 기존 문제: SQLBindParameter, SQLExecute 리턴 코드 무시
// 수정: 각 단계마다 에러 체크 및 로그 → 핸들 해제 후 반환
// ==========================================
LoginResult DBManager::ProcessLogin(const std::string& id, const std::string& pw, int input_type) {
    if (!hdbc_) return LoginResult::DB_ERROR;

    // [입력값 길이 검증]
    if (id.empty() || id.size() > 50 || pw.empty() || pw.size() > 100) {
        LOG_ERROR("DBManager", "잘못된 입력값 길이 (id:" << id.size() << ", pw:" << pw.size() << ")");
        return LoginResult::DB_ERROR;
    }

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
        return LoginResult::DB_ERROR;
    }

    // =========================================================
    // STEP 1. 계정 존재 여부 및 해시값/솔트 조회
    // =========================================================
    const char* select_sql = "SELECT password_hash, salt FROM Accounts WHERE account_id = ?";
    SQLRETURN ret = SQLPrepareA(stmt, (SQLCHAR*)select_sql, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "SELECT Prepare 실패");
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    SQLLEN id_len = SQL_NTS;
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        50, 0, (SQLPOINTER)id.c_str(), id.size(), &id_len);

    // SQLBindParameter 리턴값 검증
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "SELECT SQLBindParameter(1) 실패");
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    ret = SQLExecute(stmt);

    // SQLExecute 리턴값 검증
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "SELECT SQLExecute 실패");
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    // 결과를 담을 버퍼 준비
    SQLCHAR db_pw_hash[128] = { 0 };
    SQLCHAR db_salt[32] = { 0 };
    SQLLEN cb_pw_hash = 0, cb_salt = 0;

    ret = SQLFetch(stmt);

    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        // [계정 존재] → 1번 컬럼: hash, 2번 컬럼: salt
        SQLGetData(stmt, 1, SQL_C_CHAR, db_pw_hash, sizeof(db_pw_hash), &cb_pw_hash);
        SQLGetData(stmt, 2, SQL_C_CHAR, db_salt, sizeof(db_salt), &cb_salt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);

        std::string stored_hash(reinterpret_cast<char*>(db_pw_hash));
        std::string stored_salt(reinterpret_cast<char*>(db_salt));

        std::string attempt_hash = CryptoUtils::HashPasswordSHA256(pw, stored_salt);

        return (stored_hash == attempt_hash) ? LoginResult::SUCCESS : LoginResult::WRONG_PASSWORD;
    }

    // =========================================================
    // STEP 2. 계정 없음 → 자동 회원가입 (신규 해시 및 솔트 생성)
    // =========================================================
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    stmt = SQL_NULL_HSTMT;

    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
        return LoginResult::DB_ERROR;
    }

    std::string new_salt = CryptoUtils::GenerateSalt(16);
    std::string new_hash = CryptoUtils::HashPasswordSHA256(pw, new_salt);

    const char* insert_sql = "INSERT INTO Accounts (account_id, password_hash, salt, input) VALUES (?, ?, ?, ?)";
    ret = SQLPrepareA(stmt, (SQLCHAR*)insert_sql, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT Prepare 실패");
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    // 1번 파라미터: account_id
    SQLLEN id_len2 = SQL_NTS;
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        50, 0, (SQLPOINTER)id.c_str(), id.size(), &id_len2);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT SQLBindParameter(1: account_id) 실패");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    // 2번 파라미터: password_hash
    SQLLEN hash_len = SQL_NTS;
    ret = SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        64, 0, (SQLPOINTER)new_hash.c_str(), new_hash.size(), &hash_len);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT SQLBindParameter(2: password_hash) 실패");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    // 3번 파라미터: salt
    SQLLEN salt_len = SQL_NTS;
    ret = SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        16, 0, (SQLPOINTER)new_salt.c_str(), new_salt.size(), &salt_len);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT SQLBindParameter(3: salt) 실패");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    // 4번 파라미터: input (정수형)
    SQLLEN input_len = 0;
    ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
        0, 0, (SQLPOINTER)&input_type, sizeof(int), &input_len);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT SQLBindParameter(4: input) 실패");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    ret = SQLExecute(stmt);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        return LoginResult::NEW_REGISTERED;
    }

    LOG_ERROR("DBManager", "INSERT Execute 실패");
    return LoginResult::DB_ERROR;
}
