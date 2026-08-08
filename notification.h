#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <cstring>
#include <chrono>
#include <random>
#include "json.hpp"
#include <curl/curl.h>
#include "wdsystem.h"
#include "logger.h"

using json = nlohmann::json;

// ========== 简易 WebSocket 客户端（基于 socket，不依赖 libcurl WS API） ==========
class WDSimpleWebSocket {
private:
    std::string host;
    int port;
    std::string path;
    std::atomic<bool> connected{false};
    std::atomic<bool> running{false};
    std::thread recv_thread;
    std::function<void(const std::string&)> on_message_cb;
    std::function<void()> on_open_cb;
    std::function<void(const std::string&)> on_error_cb;

#ifdef _WIN32
    SOCKET sock = INVALID_SOCKET;
#else
    int sock = -1;
#endif

    // 生成 WebSocket 握手 key
    std::string generate_key() {
        static const char base64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        unsigned char nonce[16];
        for (int i = 0; i < 16; i++) nonce[i] = (unsigned char)dis(gen);
        std::string key;
        for (int i = 0; i < 16; i += 3) {
            key += base64[(nonce[i] >> 2) & 0x3F];
            key += base64[((nonce[i] & 0x03) << 4) | ((nonce[i+1] >> 4) & 0x0F)];
            key += base64[((nonce[i+1] & 0x0F) << 2) | ((nonce[i+2] >> 6) & 0x03)];
            key += base64[nonce[i+2] & 0x3F];
        }
        return key;
    }

    bool send_raw(const char* data, size_t len) {
#ifdef _WIN32
        return send(sock, data, (int)len, 0) == (int)len;
#else
        return send(sock, data, len, 0) == (int)len;
#endif
    }

    int recv_raw(char* buf, size_t max_len) {
#ifdef _WIN32
        return recv(sock, buf, (int)max_len, 0);
#else
        return recv(sock, buf, max_len, 0);
#endif
    }

public:
    WDSimpleWebSocket() {}
    ~WDSimpleWebSocket() { disconnect(); }

    bool connect_ws(const std::string& url, 
                    std::function<void(const std::string&)> on_msg,
                    std::function<void()> on_open = nullptr,
                    std::function<void(const std::string&)> on_err = nullptr) {
        on_message_cb = on_msg;
        on_open_cb = on_open;
        on_error_cb = on_err;

        // 解析 ws://host:port/path
        if (url.substr(0, 5) != "ws://") {
            if (on_error_cb) on_error_cb("URL must start with ws://");
            return false;
        }
        std::string rest = url.substr(5);
        size_t slash = rest.find('/');
        std::string host_port = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        path = (slash == std::string::npos) ? "/" : rest.substr(slash);

        size_t colon = host_port.find(':');
        host = (colon == std::string::npos) ? host_port : host_port.substr(0, colon);
        port = (colon == std::string::npos) ? 80 : std::stoi(host_port.substr(colon + 1));

#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) {
            if (on_error_cb) on_error_cb("socket creation failed");
            return false;
        }
#else
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            if (on_error_cb) on_error_cb("socket creation failed");
            return false;
        }
#endif

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(host.c_str());

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            if (on_error_cb) on_error_cb("connect failed");
            close_sock();
            return false;
        }

        // WebSocket 握手
        std::string key = generate_key();
        std::string request = "GET " + path + " HTTP/1.1\r\n"
                            + "Host: " + host + ":" + std::to_string(port) + "\r\n"
                            + "Upgrade: websocket\r\n"
                            + "Connection: Upgrade\r\n"
                            + "Sec-WebSocket-Key: " + key + "\r\n"
                            + "Sec-WebSocket-Version: 13\r\n"
                            + "\r\n";

        if (!send_raw(request.c_str(), request.size())) {
            if (on_error_cb) on_error_cb("handshake send failed");
            close_sock();
            return false;
        }

        // 接收响应
        char response[1024] = {0};
        int received = recv_raw(response, sizeof(response) - 1);
        if (received <= 0) {
            if (on_error_cb) on_error_cb("handshake recv failed");
            close_sock();
            return false;
        }

        std::string resp_str(response, received);
        if (resp_str.find("101") == std::string::npos || 
            resp_str.find("Switching Protocols") == std::string::npos) {
            if (on_error_cb) on_error_cb("handshake failed: " + resp_str.substr(0, 100));
            close_sock();
            return false;
        }

        connected = true;
        running = true;
        if (on_open_cb) on_open_cb();

        recv_thread = std::thread(&WDSimpleWebSocket::recv_loop, this);
        return true;
    }

    void disconnect() {
        running = false;
        connected = false;
        if (recv_thread.joinable()) {
            recv_thread.join();
        }
        close_sock();
    }

    bool send_text(const std::string& text) {
        if (!connected) return false;

        size_t len = text.size();
        std::vector<unsigned char> frame;
        frame.push_back(0x81); // FIN=1, opcode=text

        if (len < 126) {
            frame.push_back(0x80 | (unsigned char)len); // MASK=1
        } else if (len < 65536) {
            frame.push_back(0x80 | 126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; i--) {
                frame.push_back((len >> (i * 8)) & 0xFF);
            }
        }

        // Masking key
        unsigned char mask[4];
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        for (int i = 0; i < 4; i++) mask[i] = (unsigned char)dis(gen);
        frame.insert(frame.end(), mask, mask + 4);

        // Masked payload
        for (size_t i = 0; i < len; i++) {
            frame.push_back(text[i] ^ mask[i % 4]);
        }

        return send_raw((const char*)frame.data(), frame.size());
    }

    bool is_connected() const { return connected; }

private:
    void recv_loop() {
        while (running) {
            unsigned char header[2];
            int received = recv_raw((char*)header, 2);
            if (received < 2) {
                if (running) {
                    connected = false;
                    if (on_error_cb) on_error_cb("connection closed");
                }
                break;
            }

            bool fin = (header[0] & 0x80) != 0;
            unsigned char opcode = header[0] & 0x0F;
            bool masked = (header[1] & 0x80) != 0;
            unsigned long long payload_len = header[1] & 0x7F;

            if (payload_len == 126) {
                unsigned char ext[2];
                if (recv_raw((char*)ext, 2) < 2) break;
                payload_len = ((unsigned long long)ext[0] << 8) | ext[1];
            } else if (payload_len == 127) {
                unsigned char ext[8];
                if (recv_raw((char*)ext, 8) < 8) break;
                payload_len = 0;
                for (int i = 0; i < 8; i++) {
                    payload_len = (payload_len << 8) | ext[i];
                }
            }

            if (masked) {
                unsigned char mask_key[4];
                if (recv_raw((char*)mask_key, 4) < 4) break;
            }

            // 读取 payload
            std::string payload;
            if (payload_len > 0) {
                if (payload_len > 10 * 1024 * 1024) { // 限制 10MB
                    connected = false;
                    break;
                }
                payload.resize((size_t)payload_len);
                size_t total_received = 0;
                while (total_received < (size_t)payload_len) {
                    int r = recv_raw(&payload[total_received], (size_t)payload_len - total_received);
                    if (r <= 0) {
                        connected = false;
                        break;
                    }
                    total_received += r;
                }
                if (!connected) break;
            }

            if (opcode == 0x1) { // Text frame
                if (on_message_cb) on_message_cb(payload);
            } else if (opcode == 0x8) { // Close frame
                connected = false;
                break;
            } else if (opcode == 0x9) { // Ping
                // Send Pong
                unsigned char pong[2] = {0x8A, 0x00};
                send_raw((char*)pong, 2);
            }
        }
        connected = false;
    }

    void close_sock() {
#ifdef _WIN32
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
        WSACleanup();
#else
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }
#endif
    }
};

// ========== HTTP 通知客户端 ==========
class WDHttpNotify {
private:
    std::string url;
    std::string secret;
    int timeout_sec;

    static size_t write_callback(char* data, size_t size, size_t nitems, void* userdata) {
        std::string* response = static_cast<std::string*>(userdata);
        response->append(data, size * nitems);
        return size * nitems;
    }

public:
    WDHttpNotify(const std::string& notify_url, const std::string& key, int timeout = 10)
        : url(notify_url), secret(key), timeout_sec(timeout) {}

    bool send(const json& payload) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string body = payload.dump();
        std::string response_body;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth_header = "X-Auth-Key: " + secret;
        headers = curl_slist_append(headers, auth_header.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return res == CURLE_OK && (code >= 200 && code < 300);
    }
};

// ========== 通知中心 ==========
class WDNotifier {
private:
    // WS 主入口
    WDSimpleWebSocket ws_client;
    std::string ws_url;
    std::string ws_secret;
    bool ws_enabled = false;

    // HTTP 备用入口
    WDHttpNotify* http_notify = nullptr;
    std::string http_url;
    std::string http_secret;
    bool http_enabled = false;

    // 状态
    std::atomic<bool> running{false};
    std::thread reconnect_thread;

    // 喵云崽重启记录
    std::atomic<bool> restart_requested{false};
    std::mutex restart_mutex;
    std::chrono::steady_clock::time_point restart_time;

    // 崩溃图片缓存（base64）
    std::string crash_image_b64;
    bool crash_image_loaded = false;

    // 加载崩溃图片
    void load_crash_image() {
        if (crash_image_loaded) return;
        std::ifstream f("crash_image.b64");
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            crash_image_b64 = ss.str();
            crash_image_loaded = true;
            WD_LOG_INFO("崩溃图片已加载: " << crash_image_b64.length() << " chars");
        } else {
            WD_LOG_WARN("未找到崩溃图片 crash_image.b64");
        }
    }

    // 回调
    std::function<void(const json&)> on_ws_message;

public:
    WDNotifier() {}
    ~WDNotifier() { stop(); }

    // 初始化
    void init(const std::string& primary_ws_url, const std::string& primary_key,
              const std::string& backup_http_url, const std::string& backup_key) {
        ws_url = primary_ws_url;
        ws_secret = primary_key;
        http_url = backup_http_url;
        http_secret = backup_key;
        ws_enabled = !ws_url.empty();
        http_enabled = !http_url.empty();

        if (http_enabled) {
            http_notify = new WDHttpNotify(http_url, http_secret);
        }
    }

    // 启动
    void start(std::function<void(const json&)> msg_handler = nullptr) {
        on_ws_message = msg_handler;
        running = true;

        if (ws_enabled) {
            connect_ws();
            reconnect_thread = std::thread(&WDNotifier::reconnect_loop, this);
        }
    }

    // 停止
    void stop() {
        running = false;
        ws_client.disconnect();
        if (reconnect_thread.joinable()) {
            reconnect_thread.join();
        }
        if (http_notify) {
            delete http_notify;
            http_notify = nullptr;
        }
    }

    // ===== 发送通知（WS 优先，HTTP 备用） =====
    bool notify(const std::string& type, const json& data) {
        json payload;
        payload["type"] = type;
        payload["timestamp"] = wd_get_timestamp();
        payload["data"] = data;

        bool sent = false;

        // 尝试 WS
        if (ws_enabled && ws_client.is_connected()) {
            sent = ws_client.send_text(payload.dump());
        }

        // WS 失败则尝试 HTTP
        if (!sent && http_enabled && http_notify) {
            sent = http_notify->send(payload);
        }

        if (!sent) {
            WD_LOG_WARN("通知发送失败 [" << type << "]: WS=" << ws_client.is_connected()
                       << " HTTP=" << (http_notify != nullptr));
        }

        return sent;
    }

    // ===== 程序崩溃通知 =====
    bool notify_crash(const std::string& name, WDProcessId pid, int restart_count,
                       const std::string& crash_reason = "未知") {
        load_crash_image();

        json data;
        data["name"] = name;
        data["pid"] = (pid == WD_INVALID_PROCESS_ID) ? 0 : pid;
        data["restart_count"] = restart_count;
        data["action"] = "auto_restart";
        data["crash_reason"] = crash_reason;

        // 附带崩溃图片（base64）
        if (crash_image_loaded && !crash_image_b64.empty()) {
            data["image"] = crash_image_b64;
            data["image_format"] = "jpg";
        }

        WD_LOG_ERROR("程序崩溃: " << name << " (PID=" << pid << "), 原因: " << crash_reason << ", 已通报并重启");
        return notify("crash", data);
    }

    // ===== 半启动状态通知 =====
    bool notify_half_start(const std::string& name, const std::vector<int>& ports,
                           const std::vector<bool>& port_status) {
        json data;
        data["name"] = name;
        data["status"] = "half_start";
        data["ports"] = ports;
        data["port_status"] = port_status;
        data["restart_requested"] = restart_requested.load();
        WD_LOG_WARN("半启动状态: " << name);
        return notify("half_start", data);
    }

    // ===== 手动重启通知 =====
    bool notify_manual_restart(const std::string& name, const std::string& reason) {
        json data;
        data["name"] = name;
        data["reason"] = reason;
        data["action"] = "manual_restart";
        WD_LOG_INFO("手动重启: " << name << " [" << reason << "]");
        return notify("manual_restart", data);
    }

    // ===== 看门狗启动通知 =====
    bool notify_watchdog_start() {
        json data;
        data["message"] = "Cat Watchdog started";
        data["version"] = "1.0";
        return notify("watchdog_start", data);
    }

    // ===== 记录重启请求 =====
    void record_restart_request() {
        std::lock_guard<std::mutex> lock(restart_mutex);
        restart_requested = true;
        restart_time = std::chrono::steady_clock::now();
        WD_LOG_INFO("记录到重启请求（账号 1277279916）");
    }

    // ===== 检查是否有重启请求 =====
    bool has_restart_request() {
        std::lock_guard<std::mutex> lock(restart_mutex);
        if (!restart_requested) return false;
        // 5 分钟内有效
        auto elapsed = std::chrono::steady_clock::now() - restart_time;
        if (elapsed > std::chrono::minutes(5)) {
            restart_requested = false;
            return false;
        }
        return true;
    }

    // ===== 清除重启请求 =====
    void clear_restart_request() {
        std::lock_guard<std::mutex> lock(restart_mutex);
        restart_requested = false;
    }

    bool is_ws_connected() const { return ws_client.is_connected(); }

private:
    void connect_ws() {
        if (!ws_enabled || ws_client.is_connected()) return;

        bool ok = ws_client.connect_ws(
            ws_url,
            [this](const std::string& msg) {
                try {
                    json j = json::parse(msg);
                    // 检查是否是喵云崽的重启消息
                    if (j.contains("user_id") && j["user_id"].get<std::string>() == "1277279916") {
                        std::string text = j.value("message", "");
                        if (text.find("重启") != std::string::npos ||
                            text.find("restart") != std::string::npos) {
                            record_restart_request();
                        }
                    }
                    if (on_ws_message) on_ws_message(j);
                } catch (...) {
                    // 忽略非 JSON 消息
                }
            },
            [this]() {
                WD_LOG_INFO("WS 通知连接成功: " << ws_url);
                // 发送认证
                json auth;
                auth["type"] = "auth";
                auth["key"] = ws_secret;
                ws_client.send_text(auth.dump());
            },
            [this](const std::string& err) {
                WD_LOG_WARN("WS 通知错误: " << err);
            }
        );

        if (!ok) {
            WD_LOG_WARN("WS 通知连接失败: " << ws_url);
        }
    }

    void reconnect_loop() {
        while (running) {
            if (!ws_client.is_connected()) {
                wd_sleep_sec(5);
                connect_ws();
            }
            wd_sleep_sec(10);
        }
    }
};
