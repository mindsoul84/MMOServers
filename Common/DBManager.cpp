#include "DBManager.h"

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

    // =========================================================================
    // ★ MSSQL 접속 문자열 (Windows 인증, SQLEXPRESS, game_db)
    // =========================================================================
    std::wstring connStr = L"Driver={SQL Server};Server=.\\SQLEXPRESS;Database=game_db;Trusted_Connection=yes;";
    SQLWCHAR outConnStr[1024];
    SQLSMALLINT outConnStrLen;

    // 4. DB 연결 시도
    retcode = SQLDriverConnectW(
        hdbc_,
        NULL,
        (SQLWCHAR*)connStr.c_str(),
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