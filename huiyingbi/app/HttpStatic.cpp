#include "HttpStatic.h"

#include "../common/Logger.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace {

std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            const std::string hex = s.substr(i + 1, 2);
            out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string contentTypeOf(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    return "text/plain; charset=utf-8";
}

std::string makeResponse(int code, const std::string& reason, const std::string& contentType,
                         const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << " " << reason << "\r\n";
    oss << "Content-Type: " << contentType << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Cache-Control: no-store\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << body;
    return oss.str();
}

} // namespace

std::string handleStaticHttpRequest(const std::string& rawRequest, const std::string& webRoot) {
    // 解析请求行： GET /path HTTP/1.1
    const size_t lineEnd = rawRequest.find("\r\n");
    std::string requestLine =
        (lineEnd == std::string::npos) ? rawRequest : rawRequest.substr(0, lineEnd);

    std::istringstream ss(requestLine);
    std::string method, target, version;
    if (!(ss >> method >> target >> version)) {
        return makeResponse(400, "Bad Request", "text/plain; charset=utf-8", "400 Bad Request");
    }
    if (method != "GET" && method != "HEAD") {
        return makeResponse(405, "Method Not Allowed", "text/plain; charset=utf-8",
                            "405 Method Not Allowed");
    }

    // 去掉查询串、百分号解码
    const size_t q = target.find('?');
    if (q != std::string::npos) target = target.substr(0, q);
    target = percentDecode(target);

    if (target.empty() || target[0] != '/') target = "/";
    if (target == "/") target = "/index.html";

    // 目录穿越防护
    if (target.find("..") != std::string::npos) {
        LOG_WARN_MSG("拒绝可疑静态请求: " << target);
        return makeResponse(403, "Forbidden", "text/plain; charset=utf-8", "403 Forbidden");
    }

    const std::string fullPath = webRoot + target;
    std::ifstream in(fullPath, std::ios::binary);
    if (!in.is_open()) {
        LOG_WARN_MSG("静态文件不存在: " << fullPath);
        return makeResponse(404, "Not Found", "text/plain; charset=utf-8",
                            "404 Not Found: " + target);
    }

    std::ostringstream buf;
    buf << in.rdbuf();
    return makeResponse(200, "OK", contentTypeOf(fullPath), buf.str());
}
