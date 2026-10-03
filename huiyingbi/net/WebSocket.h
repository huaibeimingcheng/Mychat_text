#pragma once

#include <cstdint>
#include <string>

// 极简 WebSocket 服务端实现（RFC 6455 服务端侧所需子集）：
//   - 握手：校验 Upgrade 头，按 Sec-WebSocket-Key 计算 Sec-WebSocket-Accept
//   - 帧解析：支持 text/binary/close/ping/pong、分片（continuation）、
//             16/64 位长度、客户端掩码解码，并强制长度上限防内存攻击
//   - 组帧：服务端 → 客户端（不掩码），支持 126/127 长度
namespace websocket {

enum class OpCode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA
};

struct Frame {
    bool fin = true;
    OpCode opcode = OpCode::Text;
    std::string payload;
};

// 从 HTTP 请求文本里解析握手信息，生成 101 响应。
// 成功返回 true 并填充 response；失败返回 false 并填充 error（调用方应回 400 并关闭）。
bool buildHandshakeResponse(const std::string& httpRequest,
                            std::string& response,
                            std::string& error);

// 从 data[0..len) 里尝试解析一帧。
// 返回 >0：消耗的字节数（frame 有效）
// 返回  0：数据不完整，需要继续读
// 返回 -1：协议错误（error 里是原因），调用方应关闭连接
// maxFrameSize：单帧 payload 上限
ssize_t decodeFrame(const char* data, size_t len, Frame& frame, std::string& error,
                    size_t maxFrameSize);

// 组帧（服务端 → 客户端）
std::string encodeFrame(OpCode opcode, const std::string& payload);
std::string encodeText(const std::string& text);
std::string encodePong(const std::string& payload);
std::string encodeClose(uint16_t code, const std::string& reason);

// 生成 HTTP 400 响应（非 WebSocket 请求时使用）
std::string makeBadRequestResponse(const std::string& reason);

} // namespace websocket
