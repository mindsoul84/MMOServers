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
// [수정] DB 스키마 변경 반영 + 동시성 안전 INSERT
//
// [문제]
// DB 스레드가 2개 이상일 때 MAX(account_uid) + 1 방식은
// 두 스레드가 동시에 같은 MAX 값을 읽어 동일한 account_uid로
// INSERT를 시도하면서 PK 충돌(INSERT Execute 실패)이 발생함.
//
// [수정]
// 1. INSERT 시 ODBC 수동 트랜잭션을 사용하여
//    MAX 조회와 INSERT를 하나의 원자적 단위로 묶음.
// 2. MAX 쿼리에 WITH (UPDLOCK, HOLDLOCK) 힌트를 적용하여
//    트랜잭션이 완료될 때까지 다른 스레드의 MAX 조회를 대기시킴.
// 3. INSERT 실패 시 ROLLBACK 후 에러 반환.
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

    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
        return LoginResult::DB_ERROR;
    }

    // =========================================================
    // STEP 1. 계정 존재 여부 및 해시값/솔트/UID 조회
    // =========================================================
    const char* select_sql = "SELECT account_uid, password_hash, salt FROM Accounts WHERE account_name = ?";
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

    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "SELECT SQLBindParameter(1) 실패");
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    ret = SQLExecute(stmt);

    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "SELECT SQLExecute 실패");
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return LoginResult::DB_ERROR;
    }

    // 결과를 담을 버퍼 준비
    SQLBIGINT db_account_uid = 0;
    SQLCHAR db_pw_hash[128] = { 0 };
    SQLCHAR db_salt[32] = { 0 };
    SQLLEN cb_uid = 0, cb_pw_hash = 0, cb_salt = 0;

    ret = SQLFetch(stmt);

    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        // [계정 존재] -> 1번: account_uid, 2번: hash, 3번: salt
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

    // =========================================================
    // STEP 2. 계정 없음 -> 자동 회원가입
    //         (ODBC 트랜잭션으로 MAX+INSERT 원자성 보장)
    // =========================================================
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    stmt = SQL_NULL_HSTMT;

    // ---------------------------------------------------------
    // [수정] 수동 트랜잭션 시작 (Autocommit OFF)
    //
    // DB 스레드가 2개 이상일 때 동시에 MAX(account_uid)를 조회하면
    // 같은 값을 얻어 INSERT 시 PK 충돌이 발생하므로,
    // 트랜잭션 + 잠금 힌트로 직렬화합니다.
    // ---------------------------------------------------------
    SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
        reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_OFF)), 0);

    // ---------------------------------------------------------
    // STEP 2-1. 다음 account_uid 생성 (트랜잭션 내, 잠금 보유)
    //
    // WITH (UPDLOCK, HOLDLOCK) 힌트:
    //   UPDLOCK  - 읽기 시 업데이트 잠금을 획득하여 다른 트랜잭션의
    //              동시 읽기를 대기시킴 (Shared Lock과 달리 배타적)
    //   HOLDLOCK - 트랜잭션이 완료될 때까지 잠금을 유지
    //   -> 두 스레드가 동시에 MAX를 조회해도 하나가 COMMIT할 때까지
    //      다른 하나가 대기하므로 PK 충돌이 발생하지 않음
    // ---------------------------------------------------------
    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
        return LoginResult::DB_ERROR;
    }

    const char* max_uid_sql = "SELECT ISNULL(MAX(account_uid), 0) + 1 FROM Accounts WITH (UPDLOCK, HOLDLOCK)";
    ret = SQLExecDirectA(stmt, (SQLCHAR*)max_uid_sql, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "MAX(account_uid) 조회 실패");
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_ROLLBACK);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
        return LoginResult::DB_ERROR;
    }

    SQLBIGINT new_account_uid = 1;
    SQLLEN cb_new_uid = 0;
    ret = SQLFetch(stmt);
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        SQLGetData(stmt, 1, SQL_C_SBIGINT, &new_account_uid, sizeof(SQLBIGINT), &cb_new_uid);
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    stmt = SQL_NULL_HSTMT;

    // ---------------------------------------------------------
    // STEP 2-2. INSERT (같은 트랜잭션 내에서 실행)
    // ---------------------------------------------------------
    if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &stmt) != SQL_SUCCESS) {
        SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_ROLLBACK);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
        return LoginResult::DB_ERROR;
    }

    std::string new_salt = CryptoUtils::GenerateSalt(16);
    std::string new_hash = CryptoUtils::HashPasswordSHA256(pw, new_salt);

    const char* insert_sql = "INSERT INTO Accounts (account_uid, account_name, password_hash, salt, input) VALUES (?, ?, ?, ?, ?)";
    ret = SQLPrepareA(stmt, (SQLCHAR*)insert_sql, SQL_NTS);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT Prepare 실패");
        PrintError(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_ROLLBACK);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
        return LoginResult::DB_ERROR;
    }

    // 1번 파라미터: account_uid (BIGINT)
    SQLLEN uid_len = 0;
    ret = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT,
        0, 0, (SQLPOINTER)&new_account_uid, sizeof(SQLBIGINT), &uid_len);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT SQLBindParameter(1: account_uid) 실패");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_ROLLBACK);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
        return LoginResult::DB_ERROR;
    }

    // 2번 파라미터: account_name
    SQLLEN name_len = SQL_NTS;
    ret = SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        50, 0, (SQLPOINTER)id.c_str(), id.size(), &name_len);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT SQLBindParameter(2: account_name) 실패");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_ROLLBACK);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
        return LoginResult::DB_ERROR;
    }

    // 3번 파라미터: password_hash
    SQLLEN hash_len = SQL_NTS;
    ret = SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        64, 0, (SQLPOINTER)new_hash.c_str(), new_hash.size(), &hash_len);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT SQLBindParameter(3: password_hash) 실패");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_ROLLBACK);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
        return LoginResult::DB_ERROR;
    }

    // 4번 파라미터: salt
    SQLLEN salt_len = SQL_NTS;
    ret = SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        16, 0, (SQLPOINTER)new_salt.c_str(), new_salt.size(), &salt_len);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT SQLBindParameter(4: salt) 실패");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_ROLLBACK);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
        return LoginResult::DB_ERROR;
    }

    // 5번 파라미터: input (정수형)
    SQLLEN input_len = 0;
    ret = SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_LONG, SQL_INTEGER,
        0, 0, (SQLPOINTER)&input_type, sizeof(int), &input_len);
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        LOG_ERROR("DBManager", "INSERT SQLBindParameter(5: input) 실패");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_ROLLBACK);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
        return LoginResult::DB_ERROR;
    }

    ret = SQLExecute(stmt);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
        // 트랜잭션 커밋
        SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_COMMIT);
        SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);

        if (out_account_uid) *out_account_uid = static_cast<int64_t>(new_account_uid);
        return LoginResult::NEW_REGISTERED;
    }

    // INSERT 실패 시 롤백
    LOG_ERROR("DBManager", "INSERT Execute 실패");
    PrintError(SQL_HANDLE_DBC, hdbc_);
    SQLEndTran(SQL_HANDLE_DBC, hdbc_, SQL_ROLLBACK);
    SQLSetConnectAttr(hdbc_, SQL_ATTR_AUTOCOMMIT,
        reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_AUTOCOMMIT_ON)), 0);
    return LoginResult::DB_ERROR;
}
