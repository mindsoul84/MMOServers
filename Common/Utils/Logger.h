#pragma once
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <fstream>

// ==========================================
// 간단한 로깅 유틸리티 추가
//
// 기존 std::cout/cerr 직접 출력을 대체하여
// 로그 레벨, 타임스탬프, 스레드 안전성을 제공합니다.
//
// [사용 예]
//   LOG_INFO("GameServer", "유저 접속: " << account_id);
//   LOG_ERROR("DBManager", "DB 연결 실패");
//   LOG_WARN("Network", "패킷 크기 초과: " << size);
// ==========================================

enum class LogLevel {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERR   = 4,  // ERROR는 Windows 매크로와 충돌하므로 ERR 사용
    FATAL = 5
};

class Logger {
private:
    inline static LogLevel s_min_level_ = LogLevel::INFO;
    inline static std::mutex s_mutex_;
    inline static std::ofstream s_file_;
    inline static bool s_file_enabled_ = false;

public:
    static void SetLevel(LogLevel level) { s_min_level_ = level; }
    static LogLevel GetLevel() { return s_min_level_; }

    // 파일 로깅 활성화 (선택사항)
    static bool EnableFileLog(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(s_mutex_);
        s_file_.open(filepath, std::ios::app);
        if (s_file_.is_open()) {
            s_file_enabled_ = true;
            return true;
        }
        return false;
    }

    static void Log(LogLevel level, const std::string& tag, const std::string& message) {
        if (level < s_min_level_) return;

        std::string timestamp = GetTimestamp();
        std::string level_str = LevelToString(level);

        // 포맷: [2025-01-01 12:34:56.789] [INFO] [GameServer] 메시지
        std::ostringstream oss;
        oss << "[" << timestamp << "] [" << level_str << "] [" << tag << "] " << message << "\n";
        std::string formatted = oss.str();

        std::lock_guard<std::mutex> lock(s_mutex_);

        // 콘솔 출력
        if (level >= LogLevel::ERR) {
            std::cerr << formatted;
        } else {
            std::cout << formatted;
        }

        // 파일 출력
        if (s_file_enabled_ && s_file_.is_open()) {
            s_file_ << formatted;
            s_file_.flush();
        }
    }

private:
    static std::string GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_buf);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
            << "." << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    static const char* LevelToString(LogLevel level) {
        switch (level) {
            case LogLevel::TRACE: return "TRACE";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERR:   return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default:              return "?????";
        }
    }
};

// ==========================================
// 매크로: 스트림 문법 지원
// LOG_INFO("Tag", "값: " << value << " 완료");
// ==========================================
#define LOG_IMPL(level, tag, msg) \
    do { \
        if (level >= Logger::GetLevel()) { \
            std::ostringstream _log_oss_; \
            _log_oss_ << msg; \
            Logger::Log(level, tag, _log_oss_.str()); \
        } \
    } while(0)

#define LOG_TRACE(tag, msg) LOG_IMPL(LogLevel::TRACE, tag, msg)
#define LOG_DEBUG(tag, msg) LOG_IMPL(LogLevel::DEBUG, tag, msg)
#define LOG_INFO(tag, msg)  LOG_IMPL(LogLevel::INFO,  tag, msg)
#define LOG_WARN(tag, msg)  LOG_IMPL(LogLevel::WARN,  tag, msg)
#define LOG_ERROR(tag, msg) LOG_IMPL(LogLevel::ERR,   tag, msg)
#define LOG_FATAL(tag, msg) LOG_IMPL(LogLevel::FATAL, tag, msg)
