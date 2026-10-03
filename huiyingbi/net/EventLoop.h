#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// 一个线程一个 EventLoop（Reactor）：epoll(ET) + eventfd 唤醒 + 定时器。
//
// 线程模型：
//   - 每个 EventLoop 只能由"创建它的那个线程"运行 loop()
//   - fd 的注册/注销/事件回调全部发生在 loop 线程内，因此不需要加锁
//   - 跨线程只能通过 runInLoop/queueInLoop 投递任务
class EventLoop {
public:
    using Task = std::function<void()>;
    using EventHandler = std::function<void(uint32_t events)>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();          // 在当前线程运行事件循环，直到 quit()
    void quit();          // 线程安全
    void wakeup();        // 线程安全：唤醒 epoll_wait

    bool isInLoopThread() const;
    void assertInLoopThread() const;

    // 线程安全：若已在 loop 线程则立即执行，否则入队并在 loop 线程执行
    void runInLoop(Task cb);
    // 线程安全：总是入队
    void queueInLoop(Task cb);

    // ---- 定时器（必须在 loop 线程调用；跨线程请用 runInLoop 包一层）----
    // delayMs 后执行一次，返回可用于 cancelTimer 的 id
    uint64_t runAfter(int64_t delayMs, Task cb);

    // ---- fd 事件注册（必须在 loop 线程调用）----
    void addFd(int fd, uint32_t events, EventHandler handler);
    void modFd(int fd, uint32_t events);
    void delFd(int fd);
    bool hasFd(int fd) const;

private:
    struct TimerEntry {
        uint64_t id = 0;
        int64_t expireMs = 0;
        Task cb;
    };

    void doPendingTasks();
    void processTimers();
    void drainWakeup();
    int nextTimeoutMs() const;
    static int64_t nowMs();

    int m_epollFd = -1;
    int m_wakeupFd = -1;
    std::atomic<bool> m_quit{false};
    std::thread::id m_threadId;

    mutable std::mutex m_mutex;
    std::queue<Task> m_pendingTasks;

    std::vector<std::shared_ptr<TimerEntry>> m_timers;
    uint64_t m_nextTimerId = 1;

    std::map<int, EventHandler> m_handlers;
};

// 便捷类：在自己内部启动一个线程跑 EventLoop
class EventLoopThread {
public:
    explicit EventLoopThread(const std::string& name = std::string());
    ~EventLoopThread();

    EventLoop* start();  // 返回 loop 指针，保证 loop 已就绪
    void join();

private:
    std::string m_name;
    EventLoop* m_loop = nullptr;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};
