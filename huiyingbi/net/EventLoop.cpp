#include "EventLoop.h"

#include "../common/Logger.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <errno.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>

namespace {
constexpr int kInitialEventListSize = 64;
}

EventLoop::EventLoop() {
    // 记录所属线程：EventLoop 必须在"将要运行它"的线程里构造
    m_threadId = std::this_thread::get_id();

    m_epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (m_epollFd < 0) {
        LOG_ERROR_MSG("epoll_create1 失败: " << strerror(errno));
        abort();
    }

    m_wakeupFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeupFd < 0) {
        LOG_ERROR_MSG("eventfd 失败: " << strerror(errno));
        abort();
    }

    addFd(m_wakeupFd, EPOLLIN, [this](uint32_t) { drainWakeup(); });
}

EventLoop::~EventLoop() {
    // 关闭所有仍注册的 fd（不含 wakeupFd，它在下两行单独关）
    m_handlers.clear();
    if (m_wakeupFd >= 0) ::close(m_wakeupFd);
    if (m_epollFd >= 0) ::close(m_epollFd);
}

int64_t EventLoop::nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool EventLoop::isInLoopThread() const { return std::this_thread::get_id() == m_threadId; }

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        LOG_ERROR_MSG("在错误的线程调用了 EventLoop 接口");
        abort();
    }
}

void EventLoop::quit() {
    m_quit.store(true);
    wakeup();
}

void EventLoop::wakeup() {
    if (m_wakeupFd < 0) return;
    const uint64_t one = 1;
    ssize_t n = ::write(m_wakeupFd, &one, sizeof(one));
    (void)n; // 队列满（EWOULDBLOCK）也无所谓，说明已经处于唤醒状态
}

void EventLoop::drainWakeup() {
    uint64_t v = 0;
    while (::read(m_wakeupFd, &v, sizeof(v)) > 0) { /* 读到 EAGAIN 为止 */
    }
}

void EventLoop::runInLoop(Task cb) {
    if (!cb) return;
    if (isInLoopThread()) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Task cb) {
    if (!cb) return;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_pendingTasks.push(std::move(cb));
    }
    wakeup();
}

void EventLoop::doPendingTasks() {
    std::queue<Task> tasks;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        tasks.swap(m_pendingTasks);
    }
    while (!tasks.empty()) {
        Task& t = tasks.front();
        if (t) t();
        tasks.pop();
    }
}

uint64_t EventLoop::runAfter(int64_t delayMs, Task cb) {
    assertInLoopThread();
    if (delayMs < 0) delayMs = 0;
    auto e = std::make_shared<TimerEntry>();
    e->id = m_nextTimerId++;
    e->expireMs = nowMs() + delayMs;
    e->cb = std::move(cb);
    m_timers.push_back(e);
    return e->id;
}

int EventLoop::nextTimeoutMs() const {
    if (m_timers.empty()) return 1000; // 没有定时器：最长阻塞 1s，保证退出/唤醒及时
    int64_t minExpire = 0;
    bool first = true;
    for (const auto& e : m_timers) {
        if (!e) continue;
        if (first || e->expireMs < minExpire) {
            minExpire = e->expireMs;
            first = false;
        }
    }
    if (first) return 1000;
    int64_t diff = minExpire - nowMs();
    if (diff < 0) diff = 0;
    if (diff > 1000) diff = 1000;
    return static_cast<int>(diff);
}

void EventLoop::processTimers() {
    const int64_t now = nowMs();
    std::vector<Task> due;
    std::vector<std::shared_ptr<TimerEntry>> keep;
    keep.reserve(m_timers.size());

    for (auto& e : m_timers) {
        if (!e) continue;
        if (e->expireMs <= now) {
            if (e->cb) due.push_back(e->cb);
        } else {
            keep.push_back(e);
        }
    }
    m_timers.swap(keep); // 定时器都是一次性的；重复定时器由回调自己再 runAfter

    for (auto& t : due) {
        t(); // 回调里可能新增定时器（push 到 m_timers），已不受影响
    }
}

void EventLoop::addFd(int fd, uint32_t events, EventHandler handler) {
    assertInLoopThread();
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    if (::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR_MSG("epoll_ctl ADD fd=" << fd << " 失败: " << strerror(errno));
        return;
    }
    m_handlers[fd] = std::move(handler);
}

void EventLoop::modFd(int fd, uint32_t events) {
    assertInLoopThread();
    if (m_handlers.find(fd) == m_handlers.end()) return;
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    if (::epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        LOG_WARN_MSG("epoll_ctl MOD fd=" << fd << " 失败: " << strerror(errno));
    }
}

void EventLoop::delFd(int fd) {
    assertInLoopThread();
    if (fd == m_wakeupFd) return;
    if (::epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        LOG_DEBUG_MSG("epoll_ctl DEL fd=" << fd << " 失败: " << strerror(errno));
    }
    m_handlers.erase(fd);
}

bool EventLoop::hasFd(int fd) const { return m_handlers.find(fd) != m_handlers.end(); }

void EventLoop::loop() {
    m_threadId = std::this_thread::get_id();
    LOG_INFO_MSG("EventLoop " << m_epollFd << " 启动，线程 tid=" << std::this_thread::get_id());

    std::vector<epoll_event> events(kInitialEventListSize);

    while (!m_quit.load()) {
        const int timeout = nextTimeoutMs();
        const int n = ::epoll_wait(m_epollFd, events.data(), static_cast<int>(events.size()), timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR_MSG("epoll_wait 失败: " << strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t ev = events[i].events;

            // 拷贝一份 handler：回调里可能会 delFd 甚至销毁对象
            auto it = m_handlers.find(fd);
            if (it == m_handlers.end()) continue;
            EventHandler handler = it->second;
            handler(ev);
        }

        if (n == static_cast<int>(events.size()) && events.size() < 4096) {
            events.resize(events.size() * 2);
        }

        processTimers();
        doPendingTasks();
    }

    LOG_INFO_MSG("EventLoop " << m_epollFd << " 退出");
}

// ---------------------------------------------------------------------------
// EventLoopThread
// ---------------------------------------------------------------------------
EventLoopThread::EventLoopThread(const std::string& name) : m_name(name) {}

EventLoopThread::~EventLoopThread() {
    if (m_loop) m_loop->quit();
    join();
}

EventLoop* EventLoopThread::start() {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_thread = std::thread([this]() {
        EventLoop loop;
        {
            std::lock_guard<std::mutex> lk2(m_mutex);
            m_loop = &loop;
            m_cv.notify_all();
        }
        loop.loop();
    });
    m_cv.wait(lk, [this] { return m_loop != nullptr; });
    return m_loop;
}

void EventLoopThread::join() {
    if (m_thread.joinable()) m_thread.join();
}
