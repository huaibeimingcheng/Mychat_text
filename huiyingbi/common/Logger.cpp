#include "Logger.h"

#include <cstring>
#include <ctime>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

const char* kLevelTag[] = {"DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
const char* kLevelColor[] = {"\033[36m", "\033[32m", "\033[33m", "\033[31m", "\033[35m"};

// 仅用于取文件名（去掉长路径）
const char* shortFileName(const char* path) {
    const char* p = strrchr(path, '/');
    return p ? p + 1 : path;
}

pid_t currentTid() {
    return static_cast<pid_t>(::syscall(SYS_gettid));
}

} // namespace

const char* logLevelName(LogLevel lv) {
    int i = static_cast<int>(lv);
    if (i < 0 || i > 4) i = 4;
    return kLevelTag[i];
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    if (m_fp) fclose(m_fp);
}

bool Logger::setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_mutex);
    FILE* fp = fopen(path.c_str(), "a");
    if (!fp) return false;
    if (m_fp) fclose(m_fp);
    m_fp = fp;
    return true;
}

void Logger::flush() {
    std::lock_guard<std::mutex> lk(m_mutex);
    fflush(stderr);
    if (m_fp) fflush(m_fp);
}

void Logger::write(LogLevel lv, const char* file, int line, const std::string& msg) {
    if (static_cast<int>(lv) < static_cast<int>(m_level)) return;

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);

    char ts[64];
    // 2026-09-29 12:00:00.123
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             static_cast<int>(tv.tv_usec / 1000));

    int idx = static_cast<int>(lv);
    if (idx < 0) idx = 0;
    if (idx > 4) idx = 4;

    // 文件里不带颜色，终端带颜色
    char plain[4096];
    int n = snprintf(plain, sizeof(plain), "%s [%s] [tid=%d] [%s:%d] %s\n",
                     ts, kLevelTag[idx], static_cast<int>(currentTid()),
                     shortFileName(file), line, msg.c_str());
    if (n < 0) return;
    if (static_cast<size_t>(n) >= sizeof(plain)) n = sizeof(plain) - 1; // 超长消息截断

    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_fp) {
        fwrite(plain, 1, static_cast<size_t>(n), m_fp);
        fflush(m_fp); // 日志要能立刻看到，聊天服务器吞吐不高，直接 flush
    }
    fprintf(stderr, "%s%s\033[0m [tid=%d] [%s:%d] %s\n",
            kLevelColor[idx], ts, static_cast<int>(currentTid()),
            shortFileName(file), line, msg.c_str());
    fflush(stderr);
}
