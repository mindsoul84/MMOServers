#pragma once

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <iostream>
#include <string>
#include <functional>
#include <chrono>

// Windows: ec.message()는 CP_ACP(CP949)로 반환되므로
// UTF-8 콘솔 환경에서 깨지지 않도록 UTF-8로 변환하는 헬퍼 추가
#ifdef _WIN32
#include <windows.h>
#endif

// ==========================================
//   네트워크 에러 핸들링 유틸리티
// 재시도 로직, 에러 분류, 로깅 기능 제공
// ==========================================

namespace NetworkUtils {

    // ---------------------------------------------------------
    // [내부] ec.message() CP_ACP → UTF-8 변환 헬퍼
    // Windows에서 boost::system::error_code::message()는
    // 시스템 코드페이지(CP949) 문자열을 반환합니다.
    // /utf-8 + SetConsoleOutputCP(CP_UTF8) 환경에서는
    // CP949 바이트를 UTF-8로 변환해야 한글이 정상 출력됩니다.
    // ---------------------------------------------------------
#ifdef _WIN32
    inline std::string ErrorMessageToUtf8(const std::string& ansi_str) {
        if (ansi_str.empty()) return ansi_str;

        // 1단계: CP_ACP(CP949) → UTF-16
        int wlen = MultiByteToWideChar(
            CP_ACP, 0,
            ansi_str.c_str(), static_cast<int>(ansi_str.size()),
            nullptr, 0);
        if (wlen <= 0) return ansi_str;

        std::wstring wstr(wlen, L'\0');
        MultiByteToWideChar(
            CP_ACP, 0,
            ansi_str.c_str(), static_cast<int>(ansi_str.size()),
            &wstr[0], wlen);

        // 2단계: UTF-16 → UTF-8
        int ulen = WideCharToMultiByte(
            CP_UTF8, 0,
            wstr.c_str(), wlen,
            nullptr, 0, nullptr, nullptr);
        if (ulen <= 0) return ansi_str;

        std::string utf8_str(ulen, '\0');
        WideCharToMultiByte(
            CP_UTF8, 0,
            wstr.c_str(), wlen,
            &utf8_str[0], ulen, nullptr, nullptr);

        return utf8_str;
    }
#else
    // Linux/macOS는 이미 UTF-8이므로 변환 불필요
    inline std::string ErrorMessageToUtf8(const std::string& str) {
        return str;
    }
#endif

    // ---------------------------------------------------------
    // 에러 심각도 분류
    // ---------------------------------------------------------
    enum class ErrorSeverity {
        RECOVERABLE,    // 재시도 가능 (일시적 네트워크 문제)
        FATAL,          // 세션 종료 필요 (연결 끊김)
        IGNORED_ERROR   // 무시 가능 (operation_aborted 등)
    };

    // ---------------------------------------------------------
    // 에러 코드 분류 함수
    // ---------------------------------------------------------
    inline ErrorSeverity ClassifyError(const boost::system::error_code& ec) {
        if (!ec) return ErrorSeverity::IGNORED_ERROR;

        // 작업 취소 (정상적인 종료 과정)
        if (ec == boost::asio::error::operation_aborted) {
            return ErrorSeverity::IGNORED_ERROR;
        }

        // 연결 관련 치명적 에러
        if (ec == boost::asio::error::connection_reset ||
            ec == boost::asio::error::connection_refused ||
            ec == boost::asio::error::broken_pipe ||
            ec == boost::asio::error::not_connected ||
            ec == boost::asio::error::eof) {
            return ErrorSeverity::FATAL;
        }

        // 일시적 에러 (재시도 가능)
        if (ec == boost::asio::error::would_block ||
            ec == boost::asio::error::try_again ||
            ec == boost::asio::error::timed_out ||
            ec == boost::asio::error::host_unreachable ||
            ec == boost::asio::error::network_unreachable) {
            return ErrorSeverity::RECOVERABLE;
        }

        // 기본적으로 치명적 에러로 처리
        return ErrorSeverity::FATAL;
    }

    // ---------------------------------------------------------
    // 에러 로깅 함수
    // ---------------------------------------------------------
    inline void LogError(const std::string& context, 
                         const boost::system::error_code& ec,
                         ErrorSeverity severity) {
        const char* severity_str = "";
        switch (severity) {
            case ErrorSeverity::RECOVERABLE:  severity_str = "⚠️ RECOVERABLE"; break;
            case ErrorSeverity::FATAL:        severity_str = "🚨 FATAL";       break;
            case ErrorSeverity::IGNORED_ERROR: severity_str = "ℹ️ IGNORED";   break;
        }

        // ec.message()는 Windows에서 CP949로 반환되므로 UTF-8로 변환 후 출력
        std::cerr << "[Network] " << severity_str << " [" << context << "] "
                  << "Error: " << ErrorMessageToUtf8(ec.message())
                  << " (Code: " << ec.value() << ")\n";
    }

    // ---------------------------------------------------------
    // 에러 처리 결과
    // ---------------------------------------------------------
    struct ErrorHandleResult {
        bool should_retry;          // 재시도 해야 하는지
        bool should_disconnect;     // 세션 종료해야 하는지
        int retry_delay_ms;         // 재시도 대기 시간 (ms)
    };

    // ---------------------------------------------------------
    // 에러 처리 함수 (재시도 로직 포함)
    // ---------------------------------------------------------
    inline ErrorHandleResult HandleError(
        const std::string& context,
        const boost::system::error_code& ec,
        int current_retry_count = 0,
        int max_retries = 3) 
    {
        ErrorHandleResult result = { false, false, 0 };
        
        if (!ec) return result;  // 에러 없음

        ErrorSeverity severity = ClassifyError(ec);
        LogError(context, ec, severity);

        switch (severity) {
            case ErrorSeverity::IGNORED_ERROR:
                // 아무것도 하지 않음
                break;

            case ErrorSeverity::RECOVERABLE:
                if (current_retry_count < max_retries) {
                    result.should_retry = true;
                    // 지수 백오프: 100ms, 200ms, 400ms...
                    result.retry_delay_ms = 100 * (1 << current_retry_count);
                    std::cout << "[Network] 🔄 " << context 
                              << " 재시도 예정 (" << (current_retry_count + 1) 
                              << "/" << max_retries << ") "
                              << result.retry_delay_ms << "ms 후\n";
                } else {
                    std::cerr << "[Network] ❌ " << context 
                              << " 최대 재시도 횟수 초과. 연결 종료.\n";
                    result.should_disconnect = true;
                }
                break;

            case ErrorSeverity::FATAL:
                result.should_disconnect = true;
                break;
        }

        return result;
    }

    // ---------------------------------------------------------
    // 비동기 재시도 헬퍼 (타이머 기반)
    // ---------------------------------------------------------
    template<typename Func>
    inline void RetryAfter(
        boost::asio::io_context& io_context,
        int delay_ms,
        Func&& retry_func)
    {
        auto timer = std::make_shared<boost::asio::steady_timer>(
            io_context, 
            std::chrono::milliseconds(delay_ms)
        );

        timer->async_wait([timer, func = std::forward<Func>(retry_func)]
            (const boost::system::error_code& ec) {
                if (!ec) {
                    func();
                }
            });
    }

    // ---------------------------------------------------------
    // Send 결과 처리용 래퍼
    // ---------------------------------------------------------
    struct SendResult {
        bool success;
        std::string error_message;
        int bytes_sent;

        static SendResult Success(int bytes) {
            return { true, "", bytes };
        }

        static SendResult Failure(const std::string& msg) {
            return { false, msg, 0 };
        }
    };

} // namespace NetworkUtils
