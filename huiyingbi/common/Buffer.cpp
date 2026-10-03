#include "Buffer.h"

#include <cerrno>

ssize_t Buffer::readFd(int fd, int* savedErrno) {
    // 栈上备用空间：一次系统调用尽量把内核缓冲区读空，
    // 免得 EPOLLET 模式下因为没读干净而丢事件。
    char extrabuf[65536];

    struct iovec vec[2];
    const size_t writable = writableBytes();

    vec[0].iov_base = m_data.data() + m_write_index;
    vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    // 只有当 m_data 里完全没有可写空间时才用三段？不需要，两段足够。
    const int iovcnt = (writable == 0) ? 1 : 2;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if (n < 0) {
        if (savedErrno) *savedErrno = errno;
        return -1;
    }
    if (static_cast<size_t>(n) <= writable) {
        m_write_index += static_cast<size_t>(n);
    } else {
        m_write_index = m_data.size();
        append(extrabuf, static_cast<size_t>(n) - writable);
    }
    return n;
}
