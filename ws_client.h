#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include "json.hpp"
#include <curl/curl.h>
#include <curl/websockets.h>
#include "wdsystem.h"
#include "logger.h"

using json = nlohmann::json;

// 检查 libcurl 是否支持 WebSocket (>= 7.86.0)
#include <curl/curlver.h>
#if defined(LIBCURL_VERSION_NUM) && (LIBCURL_VERSION_NUM >= CURL_AT_LEAST_VERSION(7,86,0))
    #define WD_CURL_WS_SUPPORT 1
#else
    #define WD_CURL_WS_SUPPORT 0
    #warning "libcurl version < 7.86.0, WebSocket support disabled. OneBot will not work."
#endif

// ============ WebSocket 回调结构 ============
struct WDWebSocketCallbacks {
    std::function<void(const json&)> on_message;
    std::function<void()> on_open;
    std::function<void(int, const std::string&)> on_close;
    std::function<void(const std::string&)> on_error;
};

// ============ WebSocket 客户端 ============
class WDWebSocketClient {
private:
    std::string url;
    WDWebSocketCallbacks callbacks;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::thread ws_thread;
    std::mutex send_mutex;
    std::queue<json> send_queue;
    std::mutex queue_mutex;

    CURL* curl;

    // 解析一段原始数据，尝试抽取完整的 JSON 消息并分发
    void process_raw_data(const std::string& data) {
        if (data.empty()) return;
        try {
            size_t pos = 0;
            while (pos < data.length()) {
                size_t start = data.find('{', pos);
                if (start == std::string::npos) break;

                size_t depth = 0;
                size_t end = start;
                bool in_string = false;
                bool escaped = false;

                for (size_t i = start; i < data.length(); i++) {
                    char c = data[i];
                    if (escaped) { escaped = false; continue; }
                    if (c == '\\') { escaped = true; continue; }
                    if (c == '"' && !escaped) { in_string = !in_string; continue; }
                    if (in_string) continue;
                    if (c == '{') depth++;
                    else if (c == '}') {
                        depth--;
                        if (depth == 0) { end = i + 1; break; }
                    }
                }

                if (depth == 0 && end > start) {
                    try {
                        std::string json_str = data.substr(start, end - start);
                        json j = json::parse(json_str);
                        if (callbacks.on_message) {
                            callbacks.on_message(j);
                        }
                    } catch (...) {
                        // 解析失败，跳过
                    }
                    pos = end;
                } else {
                    break;
                }
            }
        } catch (...) {
            // 忽略
        }
    }

public:
    WDWebSocketClient() : curl(nullptr) {}
    ~WDWebSocketClient() { stop(); }

    // 初始化
    bool init(const std::string& ws_url, const WDWebSocketCallbacks& cb) {
#if !WD_CURL_WS_SUPPORT
        (void)ws_url;
        (void)cb;
        WD_LOG_WARN("WebSocket not supported: libcurl >= 7.86.0 required");
        return false;
#else
        url = ws_url;
        callbacks = cb;

        curl = curl_easy_init();
        if (!curl) {
            WD_LOG_ERROR("curl_easy_init failed");
            return false;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        // CURL_CONNECT_ONLY_WS = 2, 建立 WebSocket 连接
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_WS_OPTIONS, (long)CURLWS_RAW_MODE);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        return true;
#endif
    }

    // 连接（握手）
    bool connect() {
#if !WD_CURL_WS_SUPPORT
        return false;
#else
        if (!curl) return false;

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            if (callbacks.on_error) {
                callbacks.on_error(curl_easy_strerror(res));
            }
            return false;
        }

        connected = true;
        if (callbacks.on_open) {
            callbacks.on_open();
        }
        return true;
#endif
    }

    // 发送消息
    bool send(const json& data) {
#if !WD_CURL_WS_SUPPORT
        (void)data;
        return false;
#else
        if (!connected) return false;

        std::string msg = data.dump();
        std::lock_guard<std::mutex> lock(send_mutex);

        size_t sent = 0;
        CURLcode res = curl_ws_send(curl, msg.c_str(), msg.size(), &sent, 0, CURLWS_TEXT);
        if (res != CURLE_OK || sent != msg.size()) {
            WD_LOG_ERROR("WebSocket send failed: " << curl_easy_strerror(res));
            return false;
        }
        return true;
#endif
    }

    // 启动接收线程
    void start() {
        if (running) return;
        running = true;
        ws_thread = std::thread(&WDWebSocketClient::receive_loop, this);
    }

    // 停止
    void stop() {
        if (!running) return;
        running = false;
        connected = false;
        if (ws_thread.joinable()) {
            ws_thread.join();
        }
        if (curl) {
            curl_easy_cleanup(curl);
            curl = nullptr;
        }
    }

    bool is_connected() const { return connected; }
    bool is_running() const { return running; }

private:
    // 接收循环
    void receive_loop() {
#if !WD_CURL_WS_SUPPORT
        while (running) {
            wd_sleep_sec(1);
        }
#else
        char buffer[8192];
        size_t nread = 0;
        const struct curl_ws_frame* meta = nullptr;

        while (running) {
            if (!connected) {
                wd_sleep_sec(1);
                // 尝试重连
                if (connect()) {
                    WD_LOG_INFO("WebSocket reconnected");
                }
                continue;
            }

            CURLcode res = curl_ws_recv(curl, buffer, sizeof(buffer), &nread, &meta);
            if (res == CURLE_AGAIN) {
                wd_sleep_ms(10);
                continue;
            } else if (res != CURLE_OK) {
                WD_LOG_ERROR("WebSocket recv error: " << curl_easy_strerror(res));
                connected = false;
                if (callbacks.on_close) {
                    callbacks.on_close(res, curl_easy_strerror(res));
                }
                continue;
            }

            if (nread > 0) {
                process_raw_data(std::string(buffer, nread));
            }
            wd_sleep_ms(1);
        }
#endif
    }
};