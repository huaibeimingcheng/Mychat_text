#include "Connection.h"

#include "../common/Logger.h"

#include <chrono>
#include <cstring>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int64_t steadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

Connection::Connection(int fd, EventLoop* loop, std::string peer)
    : m_fd(fd), m_loop(loop), m_peer(std::move(peer)) {
    m_lastActiveMs = steadyNowMs();
    m_rateWindowStart = m_lastActiveMs;
}

Connection::~Connection() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    LOG_DEBUG_MSG("Connection 销毁 peer=" << m_peer);
}

void Connection::start() {
    m_loop->assertInLoopThread();

    // 用 weak_ptr 注册事件回调：进入回调时 lock() 提升为 shared_ptr，
    // 保证整个事件处理过程中对象不会被销毁（关闭时也安全）。
    std::weak_ptr<Connection> weak = shared_from_this();
    m_loop->addFd(m_fd, EPOLLIN | EPOLLET, [weak](uint32_t events) {
        if (auto self = weak.lock()) {
            self->handleEvent(events);
        }
    });

    LOG_INFO_MSG("新连接 peer=" << m_peer << " fd=" << m_fd);
    scheduleHeartbeat();
}

// ---------------------------------------------------------------------------
// 线程安全的对外接口
// ---------------------------------------------------------------------------
void Connection::sendText(const std::string& text) {
    if (text.empty()) return;
    const std::string frame = websocket::encodeText(text);
    std::weak_ptr<Connection> weak = shared_from_this();
    m_loop->runInLoop([weak, frame]() {
        if (auto self = weak.lock()) {
            self->sendInLoop(frame);
        }
    });
}

void Connection::closeAfterWrite() {
    std::weak_ptr<Connection> weak = shared_from_this();
    m_loop->runInLoop([weak]() {
        if (auto self = weak.lock()) {
            self->m_closeAfterWrite = true;
            if (self->m_outBuffer.readableBytes() == 0) {
                self->closeInLoop();
            } else {
                self->enableWriting();
            }
        }
    });
}

void Connection::setUsername(const std::string& name) {
    std::lock_guard<std::mutex> lk(m_userMutex);
    m_username = name;
}

std::string Connection::username() const {
    std::lock_guard<std::mutex> lk(m_userMutex);
    return m_username;
}

void Connection::setLoggedIn(bool v) {
    std::lock_guard<std::mutex> lk(m_userMutex);
    m_loggedIn = v;
}

bool Connection::isLoggedIn() const {
    std::lock_guard<std::mutex> lk(m_userMutex);
    return m_loggedIn;
}

bool Connection::checkRateLimit(int maxCount, int windowMs) {
    const int64_t now = steadyNowMs();
    if (now - m_rateWindowStart > windowMs) {
        m_rateWindowStart = now;
        m_rateCount = 0;
    }
    if (++m_rateCount > maxCount) return false;
    return true;
}

// ---------------------------------------------------------------------------
// 事件处理（loop 线程）
// ---------------------------------------------------------------------------
void Connection::handleEvent(uint32_t events) {
    if (m_state == State::Closed) return;

    if (events & (EPOLLHUP | EPOLLERR)) {
        LOG_DEBUG_MSG("fd=" << m_fd << " EPOLLHUP/EPOLLERR，关闭连接");
        closeInLoop();
        return;
    }

    if (events & (EPOLLIN | EPOLLRDHUP)) {
        handleRead();
    }
    if (m_state == State::Closed) return;

    if (events & EPOLLOUT) {
        handleWrite();
    }
}

void Connection::handleRead() {
    // EPOLLET：必须循环读到 EAGAIN，否则残留数据不会再触发事件
    while (true) {
        if (m_inBuffer.readableBytes() > protocol::MAX_FRAME_SIZE * 2) {
            LOG_WARN_MSG("fd=" << m_fd << " 输入缓冲过大，关闭连接");
            closeInLoop();
            return;
        }

        int savedErrno = 0;
        const ssize_t n = m_inBuffer.readFd(m_fd, &savedErrno);
        if (n > 0) {
            m_lastActiveMs = steadyNowMs();

            if (m_state == State::Handshaking) {
                // 找 HTTP 头结束标记
                const char* p = std::search(m_inBuffer.peek(),
                                            m_inBuffer.peek() + m_inBuffer.readableBytes(),
                                            "\r\n\r\n", "\r\n\r\n" + 4);
                if (p == m_inBuffer.peek() + m_inBuffer.readableBytes()) {
                    // 还没收全
                    if (m_inBuffer.readableBytes() > protocol::MAX_HTTP_HEADER) {
                        LOG_WARN_MSG("fd=" << m_fd << " 握手请求头过大，关闭连接");
                        closeInLoop();
                    }
                    return;
                }
                doHandshake();
                if (m_state != State::Open) return;
            }

            processFrames();
            if (m_state == State::Closed) return;
            continue; // 继续读，直到 EAGAIN
        }

        if (n == 0) {
            LOG_INFO_MSG("对端关闭 fd=" << m_fd << " peer=" << m_peer);
            closeInLoop();
            return;
        }

        if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
            return; // 读干净了
        }
        if (savedErrno == EINTR) continue;
        if (savedErrno == ECONNRESET) {
            LOG_INFO_MSG("连接被重置 fd=" << m_fd << " peer=" << m_peer);
        } else {
            LOG_WARN_MSG("读错误 fd=" << m_fd << " errno=" << savedErrno << " ("
                                      << strerror(savedErrno) << ")");
        }
        closeInLoop();
        return;
    }
}

void Connection::doHandshake() {
    const char* begin = m_inBuffer.peek();
    const char* end = begin + m_inBuffer.readableBytes();
    const char* pos = std::search(begin, end, "\r\n\r\n", "\r\n\r\n" + 4);
    if (pos == end) return; // 理论上不会走到（调用前已确认存在）

    const size_t headerLen = static_cast<size_t>(pos - begin) + 4;
    const std::string request(begin, headerLen);
    m_inBuffer.retrieve(headerLen);

    std::string response;
    std::string error;
    if (!websocket::buildHandshakeResponse(request, response, error)) {
        // 不是 WebSocket 握手：交给上层当普通 HTTP 请求处理（前端静态页面就靠这条路径）
        if (m_httpCallback) {
            LOG_DEBUG_MSG("fd=" << m_fd << " 非 WebSocket 请求，按静态资源处理: " << error);
            m_outBuffer.append(m_httpCallback(request));
        } else {
            LOG_WARN_MSG("fd=" << m_fd << " 握手失败: " << error);
            m_outBuffer.append(websocket::makeBadRequestResponse(error));
        }
        m_closeAfterWrite = true;
        handleWrite();
        return;
    }

    m_outBuffer.append(response);
    m_state = State::Open;
    handleWrite(); // 先保证 101 响应发出去（写缓冲里之前攒的数据排在它后面）
    LOG_INFO_MSG("WebSocket 握手成功 fd=" << m_fd << " peer=" << m_peer);

    // 握手完成，通知上层可以开始发业务消息了
    if (m_connectedCallback) {
        m_connectedCallback(shared_from_this());
    }
}

void Connection::processFrames() {
    while (m_state == State::Open) {
        websocket::Frame frame;
        std::string error;
        const ssize_t used = websocket::decodeFrame(m_inBuffer.peek(), m_inBuffer.readableBytes(),
                                                    frame, error, protocol::MAX_FRAME_SIZE);
        if (used == 0) break; // 数据不完整
        if (used < 0) {
            LOG_WARN_MSG("fd=" << m_fd << " 帧解析失败: " << error);
            // 按协议要求回一个 close 帧再关闭
            sendInLoop(websocket::encodeClose(1002, "protocol error"));
            m_closeAfterWrite = true;
            return;
        }
        m_inBuffer.retrieve(static_cast<size_t>(used));
        handleFrame(frame);
    }
}

void Connection::handleFrame(const websocket::Frame& frame) {
    switch (frame.opcode) {
        case websocket::OpCode::Text:
        case websocket::OpCode::Binary: {
            if (m_fragmented) {
                LOG_WARN_MSG("fd=" << m_fd << " 分片未结束时收到新的数据帧");
                closeInLoop();
                return;
            }
            if (frame.fin) {
                if (frame.opcode == websocket::OpCode::Binary) {
                    sendJson(jsonutil::makeErrorValue("不支持二进制消息"));
                    return;
                }
                handleDataMessage(frame.payload);
            } else {
                m_fragmented = true;
                m_fragPayload = frame.payload;
            }
            break;
        }
        case websocket::OpCode::Continuation: {
            if (!m_fragmented) {
                LOG_WARN_MSG("fd=" << m_fd << " 收到意外的 continuation 帧");
                closeInLoop();
                return;
            }
            if (m_fragPayload.size() + frame.payload.size() > protocol::MAX_FRAME_SIZE) {
                LOG_WARN_MSG("fd=" << m_fd << " 分片消息超过上限");
                closeInLoop();
                return;
            }
            m_fragPayload += frame.payload;
            if (frame.fin) {
                std::string whole;
                whole.swap(m_fragPayload);
                m_fragmented = false;
                handleDataMessage(whole);
            }
            break;
        }
        case websocket::OpCode::Ping:
            sendInLoop(websocket::encodePong(frame.payload));
            break;
        case websocket::OpCode::Pong:
            break; // m_lastActiveMs 已在 handleRead 里更新
        case websocket::OpCode::Close:
            LOG_INFO_MSG("收到 close 帧 fd=" << m_fd << " peer=" << m_peer);
            sendInLoop(websocket::encodeClose(1000, ""));
            m_closeAfterWrite = true;
            if (m_outBuffer.readableBytes() == 0) closeInLoop();
            break;
    }
}

void Connection::handleDataMessage(const std::string& text) {
    if (text.empty()) return;
    if (m_messageCallback) {
        m_messageCallback(shared_from_this(), text);
    }
}

// ---------------------------------------------------------------------------
// 写
// ---------------------------------------------------------------------------
void Connection::sendInLoop(std::string data) {
    if (m_state == State::Closed) return;

    m_outBuffer.append(data);

    // ⚠️ 握手还没完成时绝对不能往 socket 写业务数据：
    // 否则 WebSocket 帧会排到 101/HTTP 响应前面，浏览器会直接解析失败。
    // 先攒在写缓冲里，等 doHandshake() 把响应排到前面后一起发。
    if (m_state == State::Handshaking) return;

    // 背压保护：对端一直不收，写缓冲无限增长会拖垮服务器
    if (m_outBuffer.readableBytes() > protocol::MAX_WRITE_BUFFER) {
        LOG_WARN_MSG("fd=" << m_fd << " peer=" << m_peer << " 写缓冲超过 "
                           << protocol::MAX_WRITE_BUFFER << " 字节，断开连接");
        closeInLoop();
        return;
    }

    handleWrite();
}

void Connection::handleWrite() {
    if (m_state == State::Closed) return;

    while (m_outBuffer.readableBytes() > 0) {
        const ssize_t n = ::send(m_fd, m_outBuffer.peek(), m_outBuffer.readableBytes(), MSG_NOSIGNAL);
        if (n > 0) {
            m_outBuffer.retrieve(static_cast<size_t>(n));
            continue;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                enableWriting(); // ET 模式：必须主动挂 EPOLLOUT，否则剩余数据永久滞留
                return;
            }
            if (errno == EINTR) continue;
            LOG_WARN_MSG("写错误 fd=" << m_fd << " errno=" << errno << " (" << strerror(errno) << ")");
            closeInLoop();
            return;
        }
        // n == 0：发送非空数据时不应该出现
        LOG_WARN_MSG("send 返回 0，fd=" << m_fd);
        closeInLoop();
        return;
    }

    disableWriting();

    if (m_closeAfterWrite) {
        closeInLoop();
    }
}

void Connection::enableWriting() {
    if (m_writing) return;
    m_writing = true;
    m_loop->modFd(m_fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLOUT);
}

void Connection::disableWriting() {
    if (!m_writing) return;
    m_writing = false;
    m_loop->modFd(m_fd, EPOLLIN | EPOLLET | EPOLLRDHUP);
}

// ---------------------------------------------------------------------------
// 心跳 / 关闭
// ---------------------------------------------------------------------------
void Connection::scheduleHeartbeat() {
    if (m_state == State::Closed) return;
    std::weak_ptr<Connection> weak = shared_from_this();
    m_loop->runAfter(protocol::HEARTBEAT_INTERVAL_SEC * 1000, [weak]() {
        auto self = weak.lock();
        if (!self || self->m_state == State::Closed) return;

        const int64_t idleMs = steadyNowMs() - self->m_lastActiveMs;
        if (idleMs > protocol::IDLE_TIMEOUT_SEC * 1000LL) {
            LOG_INFO_MSG("fd=" << self->m_fd << " peer=" << self->m_peer << " 空闲超时("
                               << idleMs / 1000 << "s)，断开");
            self->closeInLoop();
            return;
        }
        // 主动 ping，让"NAT 悄悄断开"的连接尽快暴露
        self->sendInLoop(websocket::encodeFrame(websocket::OpCode::Ping, "hb"));
        self->scheduleHeartbeat();
    });
}

void Connection::closeInLoop() {
    if (m_state == State::Closed) return;
    m_state = State::Closed;

    disableWriting();
    m_loop->delFd(m_fd);
    if (m_fd >= 0) {
        ::shutdown(m_fd, SHUT_RDWR);
    }
    LOG_INFO_MSG("关闭连接 fd=" << m_fd << " peer=" << m_peer
                                << " user=" << (username().empty() ? "-" : username()));

    // 只通知一次；注意这里不能销毁自身，由上层（TcpServer）在事件回调返回后处理
    if (!m_closeNotified && m_closeCallback) {
        m_closeNotified = true;
        m_closeCallback(shared_from_this());
    }
}
