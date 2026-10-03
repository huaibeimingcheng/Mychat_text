#include "WebSocket.h"

#include "../common/crypto.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace websocket {
namespace {

const char* kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// 大小写不敏感地查找 HTTP 头
bool findHeader(const std::string& req, const std::string& name, std::string& value) {
    const size_t total = req.size();
    size_t pos = 0;
    // 跳过请求行
    size_t lineEnd = req.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    pos = lineEnd + 2;

    const std::string lowerName = [&] {
        std::string s = name;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        return s;
    }();

    while (pos < total) {
        lineEnd = req.find("\r\n", pos);
        if (lineEnd == std::string::npos || lineEnd == pos) break; // 空行 = 头结束
        const std::string line = req.substr(pos, lineEnd - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            if (key == lowerName) {
                std::string v = line.substr(colon + 1);
                const size_t b = v.find_first_not_of(" \t");
                const size_t e = v.find_last_not_of(" \t\r\n");
                if (b == std::string::npos) {
                    value.clear();
                } else {
                    value = v.substr(b, e - b + 1);
                }
                return true;
            }
        }
        pos = lineEnd + 2;
    }
    return false;
}

} // namespace

bool buildHandshakeResponse(const std::string& httpRequest,
                            std::string& response,
                            std::string& error) {
    // 必须是 GET
    if (httpRequest.compare(0, 4, "GET ") != 0) {
        error = "只支持 GET 方法";
        return false;
    }

    std::string upgrade, connection, key, version;
    if (!findHeader(httpRequest, "Upgrade", upgrade) ||
        upgrade.find("websocket") == std::string::npos) {
        error = "缺少 Upgrade: websocket";
        return false;
    }
    if (!findHeader(httpRequest, "Connection", connection) ||
        connection.find("Upgrade") == std::string::npos) {
        error = "缺少 Connection: Upgrade";
        return false;
    }
    if (!findHeader(httpRequest, "Sec-WebSocket-Key", key) || key.empty()) {
        error = "缺少 Sec-WebSocket-Key";
        return false;
    }
    if (findHeader(httpRequest, "Sec-WebSocket-Version", version) && version != "13") {
        error = "只支持 WebSocket 协议版本 13";
        return false;
    }

    const std::string accept = crypto::base64Encode(crypto::sha1Raw(key + kWebSocketGuid));

    response = "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    return true;
}

std::string makeBadRequestResponse(const std::string& reason) {
    std::string body = "WebSocket handshake failed: " + reason;
    std::string resp = "HTTP/1.1 400 Bad Request\r\n";
    resp += "Content-Type: text/plain; charset=utf-8\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: close\r\n\r\n";
    resp += body;
    return resp;
}

// ---------------------------------------------------------------------------
// 帧解析
// ---------------------------------------------------------------------------
ssize_t decodeFrame(const char* data, size_t len, Frame& frame, std::string& error,
                    size_t maxFrameSize) {
    if (len < 2) return 0;

    const unsigned char b0 = static_cast<unsigned char>(data[0]);
    const unsigned char b1 = static_cast<unsigned char>(data[1]);

    // RSV1..3 必须为 0（不支持扩展）
    if (b0 & 0x70) {
        error = "RSV 位非 0";
        return -1;
    }

    frame.fin = (b0 & 0x80) != 0;
    const uint8_t opcode = b0 & 0x0F;
    switch (opcode) {
        case 0x0: frame.opcode = OpCode::Continuation; break;
        case 0x1: frame.opcode = OpCode::Text; break;
        case 0x2: frame.opcode = OpCode::Binary; break;
        case 0x8: frame.opcode = OpCode::Close; break;
        case 0x9: frame.opcode = OpCode::Ping; break;
        case 0xA: frame.opcode = OpCode::Pong; break;
        default:
            error = "不支持的 opcode=" + std::to_string(opcode);
            return -1;
    }

    const bool masked = (b1 & 0x80) != 0;
    if (!masked) {
        // 客户端发往服务端的帧必须掩码（RFC 6455 5.1）
        error = "客户端帧未掩码";
        return -1;
    }

    uint64_t payloadLen = b1 & 0x7F;
    size_t pos = 2;

    if (payloadLen == 126) {
        if (len < pos + 2) return 0;
        payloadLen = (static_cast<uint64_t>(static_cast<unsigned char>(data[pos])) << 8) |
                     static_cast<unsigned char>(data[pos + 1]);
        pos += 2;
    } else if (payloadLen == 127) {
        if (len < pos + 8) return 0;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLen = (payloadLen << 8) | static_cast<unsigned char>(data[pos + i]);
        }
        pos += 8;
        if (payloadLen >> 63) {
            error = "长度字段非法";
            return -1;
        }
    }

    // 控制帧必须 FIN 且 payload <= 125
    const bool isControl = (opcode & 0x8) != 0;
    if (isControl && (!frame.fin || payloadLen > 125)) {
        error = "控制帧格式非法";
        return -1;
    }

    if (payloadLen > maxFrameSize) {
        error = "帧长度 " + std::to_string(payloadLen) + " 超过上限 " + std::to_string(maxFrameSize);
        return -1;
    }

    if (len < pos + 4) return 0;
    unsigned char maskKey[4];
    memcpy(maskKey, data + pos, 4);
    pos += 4;

    if (len < pos + payloadLen) return 0; // 还不完整

    frame.payload.assign(data + pos, static_cast<size_t>(payloadLen));
    for (size_t i = 0; i < frame.payload.size(); ++i) {
        frame.payload[i] = static_cast<char>(static_cast<unsigned char>(frame.payload[i]) ^
                                             maskKey[i % 4]);
    }

    return static_cast<ssize_t>(pos + payloadLen);
}

// ---------------------------------------------------------------------------
// 组帧
// ---------------------------------------------------------------------------
std::string encodeFrame(OpCode opcode, const std::string& payload) {
    std::string out;
    out.reserve(payload.size() + 10);

    out.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode))); // FIN=1

    const size_t len = payload.size();
    if (len < 126) {
        out.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }

    out += payload; // 服务端 → 客户端不掩码
    return out;
}

std::string encodeText(const std::string& text) { return encodeFrame(OpCode::Text, text); }

std::string encodePong(const std::string& payload) { return encodeFrame(OpCode::Pong, payload); }

std::string encodeClose(uint16_t code, const std::string& reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload += reason;
    if (payload.size() > 125) payload.resize(125);
    return encodeFrame(OpCode::Close, payload);
}

} // namespace websocket
