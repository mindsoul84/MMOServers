#include "DBManager.h"
#include "..\ConfigManager.h"
#include "..\Utils\CryptoUtils.h"
#include "..\Utils\Logger.h"
#include <iostream>
#include <memory>

// raw 포인터 -> unique_ptr 로 변경
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
// [수정] 모든 SQL Query를 Stored Procedure 호출로 전환
//
// [변경 전]
// - 직접 SQL SELECT/INSERT 문자열을 소스코드에서 사용
// - 파라미터별 개별 에러 체크로 중복 코드 다수
// - C++ ODBC 수동 트랜잭션(AUTOCOMMIT OFF/ON)으로 동시성 제어
//
// [변경 후]
// - 모든 DB 호출을 SP({CALL ...})로 통일. 소스코드에 SQL 쿼리문 없음
//   STEP 1: Accounts_Login_Game    — 계정 조회
//   STEP 2-1: Accounts_NextUid_Game  — 다음 account_uid 조회
//   STEP 2-3: Accounts_Register_Game — 신규 등록 (INSERT)
// - 파라미터 바인딩 개별 에러 체크 제거. SQLExecute 결과로 일괄 판단
//   SP 내부 TRY/CATCH에서 에러를 DBError 테이블에 기록 후 THROW로 재전파
// - C++ 코드에서 ODBC 수동 트랜잭션 관리 코드 완전 제거
// ==========================================
LoginResult DBManager::ProcessLogin(const std::string& id, const std::string& pw, int input_type, int64_t* out_account_uid) {
    if (!hdbc_) return LoginResult::DB_ERROR;

    // [입력값 길이 검증]
    if (id.empty() || id.size() > 50 || pw.empty() || pw.size() > 100) {
        LOG_ERROR("DBManager", "잘못된 입력값 길이 (id:" << id.size() << ", pw:" << pw.size() << ")");
        return LoginResult::DB_ERROR;
    }

    // 출력 파라미터 초기화
    if (out_account_uid) *out_account_uid = 0;

    // =========================================================
    // STEP 1. Accounts_Login_Game SP로 계정 존재 여부 조회
    //         (account_uid, password_hash, salt 반환)
    // =========================================================
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
            return LoginResult::DB_ERROR;
        }

        const char* login_sp = "{CALL Accounts_Login_Game(?)}";
        SQLLEN id_len = SQL_NTS;

        SQLPrepareA(stmt, (SQLCHAR*)login_sp, SQL_NTS);
        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
            50, 0, (SQLPOINTER)id.c_str(), id.size(), &id_len);

        SQLRETURN ret = SQLExecute(stmt);
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
            LOG_ERROR("DBManager", "Accounts_Login_Game 실행 실패");
            PrintError(SQL_HANDLE_STMT, stmt);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return LoginResult::DB_ERROR;
        }

        SQLBIGINT db_account_uid = 0;
        SQLCHAR db_pw_hash[128] = { 0 };
        SQLCHAR db_salt[32] = { 0 };
        SQLLEN cb_uid = 0, cb_pw_hash = 0, cb_salt = 0;

        ret = SQLFetch(stmt);

        if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
            // [계정 존재] -> 비밀번호 검증
            SQLGetData(stmt, 1, SQL_C_SBIGINT, &db_account_uid, sizeof(SQLBIGINT), &cb_uid);
            SQLGetData(stmt, 2, SQL_C_CHAR, db_pw_hash, sizeof(db_pw_hash), &cb_pw_hash);
            SQLGetData(stmt, 3, SQL_C_CHAR, db_salt, sizeof(db_salt), &cb_salt);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);

            std::string stored_hash(reinterpret_cast<char*>(db_pw_hash));
            std::string stored_salt(reinterpret_cast<char*>(db_salt));
            std::string attempt_hash = CryptoUtils::HashPasswordSHA256(pw, stored_salt);

            if (stored_hash == attempt_hash) {
                if (out_account_uid) *out_account_uid = static_cast<int64_t>(db_account_uid);
                return LoginResult::SUCCESS;
            }
            return LoginResult::WRONG_PASSWORD;
        }

        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // =========================================================
    // STEP 2. 계정 없음 -> 자동 회원가입
    // =========================================================

    // ---------------------------------------------------------
    // STEP 2-1. Accounts_NextUid_Game SP로 다음 account_uid 조회
    // ---------------------------------------------------------
    SQLBIGINT new_account_uid = 1;
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
            return LoginResult::DB_ERROR;
        }

        const char* next_uid_sp = "{CALL Accounts_NextUid_Game}";
        SQLRETURN ret = SQLExecDirectA(stmt, (SQLCHAR*)next_uid_sp, SQL_NTS);
        if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
            LOG_ERROR("DBManager", "Accounts_NextUid_Game 실행 실패");
            PrintError(SQL_HANDLE_STMT, stmt);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return LoginResult::DB_ERROR;
        }

        SQLLEN cb_new_uid = 0;
        ret = SQLFetch(stmt);
        if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
            SQLGetData(stmt, 1, SQL_C_SBIGINT, &new_account_uid, sizeof(SQLBIGINT), &cb_new_uid);
        }

        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }

    // ---------------------------------------------------------
    // STEP 2-2. password_hash, salt 생성 (C++ 코드에서 수행)
    // ---------------------------------------------------------
    std::string new_salt = CryptoUtils::GenerateSalt(16);
    std::string new_hash = CryptoUtils::HashPasswordSHA256(pw, new_salt);

    // ---------------------------------------------------------
    // STEP 2-3. Accounts_Register_Game SP로 INSERT 실행
    //
    // 파라미터 바인딩 에러나 PK 중복 등의 에러는
    // SP 내부 TRY/CATCH에서 DBError 테이블에 기록한 뒤
    // THROW로 재전파하여 SQLExecute 반환값으로 감지합니다.
    // ---------------------------------------------------------
    {
        SQLHSTMT stmt = SQL_NULL_HSTMT;
        if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
            return LoginResult::DB_ERROR;
        }

        const char* register_sp = "{CALL Accounts_Register_Game(?, ?, ?, ?, ?)}";
        SQLPrepareA(stmt, (SQLCHAR*)register_sp, SQL_NTS);

        SQLLEN uid_len = 0, name_len = SQL_NTS, hash_len = SQL_NTS, salt_len = SQL_NTS, input_len = 0;
        SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &new_account_uid, sizeof(SQLBIGINT), &uid_len);
        SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 50, 0, (SQLPOINTER)id.c_str(), id.size(), &name_len);
        SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 64, 0, (SQLPOINTER)new_hash.c_str(), new_hash.size(), &hash_len);
        SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 16, 0, (SQLPOINTER)new_salt.c_str(), new_salt.size(), &salt_len);
        SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER, 0, 0, (SQLPOINTER)&input_type, sizeof(int), &input_len);

        SQLRETURN ret = SQLExecute(stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);

        if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
            if (out_account_uid) *out_account_uid = static_cast<int64_t>(new_account_uid);
            return LoginResult::NEW_REGISTERED;
        }

        LOG_ERROR("DBManager", "Accounts_Register_Game 실행 실패 (에러 상세는 DBError 테이블 참조)");
        PrintError(SQL_HANDLE_DBC, hdbc_);
        return LoginResult::DB_ERROR;
    }
}
