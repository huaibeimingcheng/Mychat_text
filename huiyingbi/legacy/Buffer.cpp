#include "Buffer.h"

Buffer::Buffer(int initSize)
    : m_data(initSize), m_read_index(0), m_write_index(0) {}

size_t Buffer::readableBytes() const { return m_write_index - m_read_index; }
size_t Buffer::writableBytes() const { return m_data.size() - m_write_index; }
const char* Buffer::peek() const { return m_data.data() + m_read_index; }

void Buffer::retrieve(size_t len) {
    if (len > readableBytes()) return;
    m_read_index += len;
    // 如果数据全读完了，两个指针直接归零（低成本清空缓冲区）
    if (m_read_index == m_write_index) {
        m_read_index = 0;
        m_write_index = 0;
    }
}

void Buffer::append(const char* data, size_t len) {
    if (len == 0) return;

    if (writableBytes() < len) {
        size_t readable = readableBytes();

        // ⚠️ 关键：搬移"未读数据到最前面"能腾出的空间就是 writable（= size - readable），
        // 它本来就 < len，光搬移根本不够用。
        // 所以判定必须是"搬移后的总容量 size 是否 >= readable + len"，否则必须扩容。
        // （老写法 `readable + writable >= len` 恒为真，搬移后照样越界写，踩坏堆。）
        if (m_data.size() < readable + len) {
            m_data.resize(readable + len);
        }

        // 把未读数据整体搬到最前面；目标在前、源在后，std::copy 处理重叠是安全的
        std::copy(m_data.begin() + m_read_index, m_data.begin() + m_write_index, m_data.begin());
        m_read_index = 0;
        m_write_index = readable;
    }

    // 走到这里一定满足 writableBytes() >= len
    std::copy(data, data + len, m_data.begin() + m_write_index);
    m_write_index += len;
}

ssize_t Buffer::readFd(int fd) {
    char extrabuf[65536]; // 栈上备用空间，防止频繁触发底层扩容
    struct iovec vec[2];
    size_t writable = writableBytes();

    // vec[0] 指向 m_data 现有的尾部空闲空间
    vec[0].iov_base = m_data.data() + m_write_index;
    vec[0].iov_len = writable;
    // vec[1] 指向栈上的 extrabuf
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    ssize_t n = ::readv(fd, vec, 2);
    if (n < 0) {
        return -1;
    } else if (static_cast<size_t>(n) <= writable) {
        m_write_index += n;
    } else {
        m_write_index = m_data.size();
        append(extrabuf, n - writable);
    }
    return n;
}