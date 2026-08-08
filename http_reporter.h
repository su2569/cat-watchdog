#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include "json.hpp"
#include <curl/curl.h>
#include "wdsystem.h"
#include "logger.h"
#include "config.h"

using json = nlohmann::json;

// ============ HTTP 上报配置 ============
struct WDHttpReporterConfig {
    bool enabled;
    std::string url;
    std::string method;  // POST / PUT
    std::map<std::string, std::string> headers;
    int timeout_sec;
    int retry_count;
    int retry_delay_sec;
    bool report_on_start;
    bool report_on_change;
    int report_interval_sec;
    
    WDHttpReporterConfig() : enabled(false), method("POST"), timeout_sec(10),
                             retry_count(3), retry_delay_sec(5),
                             report_on_start(true), report_on_change(true),
                             report_interval_sec(60) {}
};

// ============ HTTP 响应结构 ============
struct WDHttpResponse {
    int code;
    std::string body;
    std::string error;
    bool success;
    
    WDHttpResponse() : code(0), success(false) {}
};

// ============ HTTP 上报器 ============
class WDHttpReporter {
private:
    WDHttpReporterConfig config;
    std::atomic<bool> running{false};
    std::atomic<bool> initialized{false};
    std::thread report_thread;
    std::mutex queue_mutex;
    std::queue<json> report_queue;
    std::map<std::string, bool> last_process_status;
    std::mutex status_mutex;
    
    // curl 写回调
    static size_t write_callback(char* data, size_t size, size_t nitems, void* userdata) {
        std::string* response = static_cast<std::string*>(userdata);
        size_t total = size * nitems;
        response->append(data, total);
        return total;
    }
    
public:
    WDHttpReporter() {}
    ~WDHttpReporter() { stop(); }
    
    // 初始化
    bool init(const WDHttpReporterConfig& cfg) {
        config = cfg;
        if (!config.enabled) {
            WD_LOG_INFO("HTTP 上报已禁用");
            initialized = true;
            return true;
        }
        
        if (config.url.empty()) {
            WD_LOG_ERROR("HTTP 上报 URL 为空");
            return false;
        }
        
        initialized = true;
        WD_LOG_INFO("HTTP 上报已初始化: " << config.url);
        return true;
    }
    
    // 启动上报线程
    void start() {
        if (!config.enabled || !initialized) return;
        if (running) return;
        
        running = true;
        report_thread = std::thread(&WDHttpReporter::report_loop, this);
        WD_LOG_INFO("HTTP 上报线程已启动");
    }
    
    // 停止
    void stop() {
        if (!running) return;
        running = false;
        if (report_thread.joinable()) {
            report_thread.join();
        }
        WD_LOG_INFO("HTTP 上报线程已停止");
    }
    
    // ===== 上报状态 =====
    bool report_status(const std::vector<WDProcessConfig>& processes) {
        if (!config.enabled || !initialized) return false;
        
        int running_count = 0;
        json process_list = json::array();
        
        for (const auto& pc : processes) {
            if (pc.is_running) running_count++;
            
            json j;
            j["name"] = pc.name;
            j["running"] = pc.is_running;
            j["pid"] = (pc.pid == WD_INVALID_PROCESS_ID) ? 0 : pc.pid;
            j["restart_count"] = pc.restart_count;
            j["cmd"] = pc.cmd;
            process_list.push_back(j);
        }
        
        WDSystemInfo sys_info = wd_get_system_info();
        
        json payload;
        payload["type"] = "status";
        payload["timestamp"] = wd_get_timestamp();
        payload["hostname"] = sys_info.hostname;
        payload["os"] = sys_info.os_name;
        payload["running_count"] = running_count;
        payload["total_count"] = processes.size();
        payload["processes"] = process_list;
        payload["watchdog_pid"] = sys_info.pid;
        
        return send(payload);
    }
    
    // ===== 上报进程变化 =====
    void report_process_change(const std::string& name, bool now_running,
                               int restart_count = 0, WDProcessId pid = WD_INVALID_PROCESS_ID) {
        if (!config.enabled || !config.report_on_change) return;
        
        std::lock_guard<std::mutex> lock(status_mutex);
        auto it = last_process_status.find(name);
        bool last_running = (it != last_process_status.end()) ? it->second : false;
        
        if (last_running == now_running) return;
        last_process_status[name] = now_running;
        
        json payload;
        payload["type"] = "process_change";
        payload["timestamp"] = wd_get_timestamp();
        payload["name"] = name;
        payload["running"] = now_running;
        payload["pid"] = (pid == WD_INVALID_PROCESS_ID) ? 0 : pid;
        payload["restart_count"] = restart_count;
        payload["status"] = now_running ? "started" : "stopped";
        
        send(payload);
    }
    
    // ===== 上报看门狗启动 =====
    void report_start(const std::vector<WDProcessConfig>& processes) {
        if (!config.enabled || !config.report_on_start) return;
        
        WDSystemInfo sys_info = wd_get_system_info();
        
        json payload;
        payload["type"] = "start";
        payload["timestamp"] = wd_get_timestamp();
        payload["hostname"] = sys_info.hostname;
        payload["os"] = sys_info.os_name;
        payload["process_count"] = processes.size();
        payload["watchdog_pid"] = sys_info.pid;
        payload["message"] = "看门狗已启动";
        
        send(payload);
    }
    
    // ===== 上报看门狗停止 =====
    void report_stop() {
        if (!config.enabled) return;
        
        json payload;
        payload["type"] = "stop";
        payload["timestamp"] = wd_get_timestamp();
        payload["message"] = "看门狗已停止";
        
        send(payload);
    }
    
    // ===== 上报自定义事件 =====
    void report_event(const std::string& event_type, const json& data) {
        if (!config.enabled) return;
        
        json payload;
        payload["type"] = event_type;
        payload["timestamp"] = wd_get_timestamp();
        payload["data"] = data;
        
        send(payload);
    }
    
    // ===== 发送上报（加入队列） =====
    bool send(const json& data) {
        if (!config.enabled || !initialized) return false;
        
        std::lock_guard<std::mutex> lock(queue_mutex);
        report_queue.push(data);
        return true;
    }
    
    bool is_running() const { return running; }
    bool is_enabled() const { return config.enabled; }
    
private:
    // ===== 上报循环 =====
    void report_loop() {
        int heartbeat_counter = 0;
        
        while (running) {
            // 处理队列中的上报
            process_queue();
            
            // 定时上报状态
            heartbeat_counter++;
            if (heartbeat_counter >= config.report_interval_sec) {
                heartbeat_counter = 0;
                // 由主程序调用 report_status，这里只发心跳
                json heartbeat;
                heartbeat["type"] = "heartbeat";
                heartbeat["timestamp"] = wd_get_timestamp();
                heartbeat["queue_size"] = get_queue_size();
                send(heartbeat);
            }
            
            wd_sleep_sec(1);
        }
        
        // 停止前处理剩余队列
        process_queue();
    }
    
    // ===== 处理队列 =====
    void process_queue() {
        while (true) {
            json data;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (report_queue.empty()) break;
                data = report_queue.front();
                report_queue.pop();
            }
            
            // 发送 HTTP 请求
            WDHttpResponse response = do_request(data.dump());
            if (!response.success) {
                WD_LOG_ERROR("HTTP 上报失败: " << response.error);
                // 可以重新入队，但为了防止无限循环，这里丢弃
            }
        }
    }
    
    // ===== 执行 HTTP 请求 =====
    WDHttpResponse do_request(const std::string& body) {
        WDHttpResponse response;
        
        CURL* curl = curl_easy_init();
        if (!curl) {
            response.error = "curl_easy_init 失败";
            return response;
        }
        
        std::string response_body;
        
        curl_easy_setopt(curl, CURLOPT_URL, config.url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, config.timeout_sec);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        
        // 设置方法
        if (config.method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        } else if (config.method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        } else {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, config.method.c_str());
        }
        
        // 设置 Body
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
        
        // 设置 Headers
        struct curl_slist* headers = nullptr;
        for (const auto& [key, value] : config.headers) {
            std::string header = key + ": " + value;
            headers = curl_slist_append(headers, header.c_str());
        }
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        // 执行请求
        CURLcode res = curl_easy_perform(curl);
        
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.code);
            response.body = response_body;
            response.success = true;
        } else {
            response.error = curl_easy_strerror(res);
            response.success = false;
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        return response;
    }
    
    size_t get_queue_size() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        return report_queue.size();
    }
};
