#pragma once

#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>

// 日志级别
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, FATAL = 4 };

const char* logLevelName(LogLevel lv);

// 线程安全的日志器：
//   - 同时输出到 stderr（带颜色）和可选的文件
//   - 每行包含 日期时间.毫秒 / 级别 / 线程号 / 源文件:行号
//   - 级别低于 setLevel 的日志会被丢弃（热路径上几乎没有开销）
class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel lv) { m_level = lv; }
    LogLevel level() const { return m_level; }

    // 打开日志文件（追加模式）。失败返回 false。
    bool setLogFile(const std::string& path);
    void flush();

    void write(LogLevel lv, const char* file, int line, const std::string& msg);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel  m_level = LogLevel::DEBUG;
    std::mutex m_mutex;
    FILE*     m_fp = nullptr; // 文件输出（可为空）
};

// 便捷宏：LOG_INFO("user " << name << " login")
#define LOG_DEBUG_MSG(msg)                                                       \
    do {                                                                         \
        if (Logger::instance().level() <= LogLevel::DEBUG) {                     \
            std::ostringstream _log_oss;                                         \
            _log_oss << msg;                                                     \
            Logger::instance().write(LogLevel::DEBUG, __FILE__, __LINE__, _log_oss.str()); \
        }                                                                        \
    } while (0)

#define LOG_INFO_MSG(msg)                                                        \
    do {                                                                         \
        if (Logger::instance().level() <= LogLevel::INFO) {                      \
            std::ostringstream _log_oss;                                         \
            _log_oss << msg;                                                     \
            Logger::instance().write(LogLevel::INFO, __FILE__, __LINE__, _log_oss.str()); \
        }                                                                        \
    } while (0)

#define LOG_WARN_MSG(msg)                                                        \
    do {                                                                         \
        if (Logger::instance().level() <= LogLevel::WARN) {                      \
            std::ostringstream _log_oss;                                         \
            _log_oss << msg;                                                     \
            Logger::instance().write(LogLevel::WARN, __FILE__, __LINE__, _log_oss.str()); \
        }                                                                        \
    } while (0)

#define LOG_ERROR_MSG(msg)                                                       \
    do {                                                                         \
        std::ostringstream _log_oss;                                             \
        _log_oss << msg;                                                         \
        Logger::instance().write(LogLevel::ERROR, __FILE__, __LINE__, _log_oss.str()); \
    } while (0)
