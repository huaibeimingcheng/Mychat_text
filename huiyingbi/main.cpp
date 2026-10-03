#include "app/ChatServer.h"
#include "common/Logger.h"
#include "net/EventLoop.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

EventLoop* g_loop = nullptr;

// 信号处理器里只做"异步信号安全"的操作：置标志 + write(eventfd)
void onSignal(int) {
    if (g_loop) g_loop->quit();
}

void printUsage(const char* prog) {
    std::cout << "用法: " << prog << " [选项]\n"
              << "  -b, --bind <地址>      监听地址（默认 0.0.0.0，即所有网卡；仅本机用 127.0.0.1）\n"
              << "  -p, --port <端口>      监听端口（默认 8888）\n"
              << "  -t, --threads <数量>   IO 线程数（默认 4）\n"
              << "  -d, --db <路径>        用户库文件（默认 data/users.db）\n"
              << "  -w, --web <目录>       前端静态目录（默认 web）\n"
              << "  -l, --log <文件>       日志文件（默认 logs/server.log）\n"
              << "  -L, --level <级别>     debug|info|warn|error（默认 info）\n"
              << "  -h, --help             显示帮助\n";
}

// 列出本机所有非回环 IPv4 地址，方便把局域网访问地址打印给用户
std::vector<std::string> localIPv4Addresses() {
    std::vector<std::string> result;
    struct ifaddrs* ifaddr = nullptr;
    if (::getifaddrs(&ifaddr) != 0) return result;

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
        char buf[INET_ADDRSTRLEN] = {0};
        if (!::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
        const std::string ip(buf);
        if (ip.rfind("127.", 0) == 0) continue; // 跳过回环
        result.push_back(ip);
    }
    ::freeifaddrs(ifaddr);
    return result;
}

bool parseLevel(const std::string& s, LogLevel& out) {
    if (s == "debug") { out = LogLevel::DEBUG; return true; }
    if (s == "info")  { out = LogLevel::INFO;  return true; }
    if (s == "warn")  { out = LogLevel::WARN;  return true; }
    if (s == "error") { out = LogLevel::ERROR; return true; }
    return false;
}

// 确保文件的父目录存在（例如 logs/server.log 需要先有 logs/）
void ensureParentDir(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;

    // 逐级创建，支持 a/b/c 这种多级路径
    std::string cur;
    for (size_t i = 0; i < slash; ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' && cur.size() > 1) {
            ::mkdir(cur.c_str(), 0755); // 已存在返回 EEXIST，忽略
        }
    }
    ::mkdir(cur.c_str(), 0755);
}

} // namespace

int main(int argc, char** argv) {
    ChatServer::Options opt;
    std::string logFile = "logs/server.log";
    LogLevel level = LogLevel::INFO;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "参数 " << what << " 缺少取值\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "-b" || arg == "--bind") {
            opt.bindAddr = next("--bind");
        } else if (arg == "-p" || arg == "--port") {
            opt.port = std::atoi(next("--port").c_str());
        } else if (arg == "-t" || arg == "--threads") {
            opt.ioThreads = std::atoi(next("--threads").c_str());
        } else if (arg == "-d" || arg == "--db") {
            opt.userDbPath = next("--db");
        } else if (arg == "-w" || arg == "--web") {
            opt.webRoot = next("--web");
        } else if (arg == "-l" || arg == "--log") {
            logFile = next("--log");
        } else if (arg == "-L" || arg == "--level") {
            const std::string lv = next("--level");
            if (!parseLevel(lv, level)) {
                std::cerr << "未知日志级别: " << lv << "\n";
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "未知参数: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (opt.port <= 0 || opt.port > 65535) {
        std::cerr << "端口不合法: " << opt.port << "\n";
        return 1;
    }
    if (opt.bindAddr.empty()) {
        std::cerr << "监听地址不能为空\n";
        return 1;
    }
    if (opt.ioThreads < 1 || opt.ioThreads > 64) {
        std::cerr << "IO 线程数不合法（1~64）: " << opt.ioThreads << "\n";
        return 1;
    }

    Logger::instance().setLevel(level);
    ensureParentDir(logFile);
    if (!Logger::instance().setLogFile(logFile)) {
        std::cerr << "警告: 无法写入日志文件 " << logFile << "，仅输出到终端\n";
    }

    LOG_INFO_MSG("==========================================");
    LOG_INFO_MSG("  MyChatServer 启动中...");
    LOG_INFO_MSG("==========================================");

    EventLoop mainLoop; // 主线程：只负责 accept
    g_loop = &mainLoop;

    std::signal(SIGPIPE, SIG_IGN); // 统一用 MSG_NOSIGNAL，这里再兜一层
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    ChatServer server(&mainLoop, opt);

    // 用 runInLoop 保证 start() 在 loop 线程执行（注册监听 fd 需要在 loop 线程）
    bool started = false;
    mainLoop.runInLoop([&]() { started = server.start(); });
    if (!started) {
        LOG_ERROR_MSG("服务器启动失败，请检查上方错误信息（监听地址是否合法、端口是否被占用）");
        return 1;
    }

    LOG_INFO_MSG("监听地址 " << opt.bindAddr << ":" << opt.port << "，IO 线程 " << opt.ioThreads);
    if (opt.bindAddr == "0.0.0.0") {
        LOG_INFO_MSG("本机访问: http://127.0.0.1:" << opt.port << "/");
        const std::vector<std::string> ips = localIPv4Addresses();
        if (ips.empty()) {
            LOG_INFO_MSG("未检测到其它网卡地址（外部机器无法访问）");
        } else {
            for (const auto& ip : ips) {
                LOG_INFO_MSG("局域网/外部访问: http://" << ip << ":" << opt.port << "/");
            }
        }
    } else {
        LOG_INFO_MSG("前端页面: http://" << opt.bindAddr << ":" << opt.port << "/");
    }
    LOG_INFO_MSG("用户库: " << opt.userDbPath << "，前端目录: " << opt.webRoot);
    LOG_INFO_MSG("按 Ctrl+C 退出");

    mainLoop.loop();

    LOG_INFO_MSG("正在关闭服务器...");
    server.stop();
    LOG_INFO_MSG("服务器已退出");
    return 0;
}
