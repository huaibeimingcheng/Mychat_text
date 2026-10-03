#pragma once

#include <string>

// 把浏览器的普通 HTTP GET 请求映射成静态文件响应。
// 用于让前端页面和 WebSocket 共用同一个端口：
//   浏览器打开 http://<host>:<port>/  → 返回 web/index.html
//   WebSocket 握手（带 Upgrade 头）    → 走协议升级
std::string handleStaticHttpRequest(const std::string& rawRequest, const std::string& webRoot);
