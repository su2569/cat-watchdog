#pragma once

#include <string>
#include <functional>
#include <map>
#include <atomic>
#include <memory>
#include <ctime>
#include <mutex>
#include "json.hpp"
#include "wdsystem.h"
#include "logger.h"
#include "ws_client.h"
#include "config.h"

using json = nlohmann::json;

// ============ OneBot 事件类型 ============
enum WDOneBotEventType {
    WD_OB_EVENT_MESSAGE = 0,
    WD_OB_EVENT_NOTICE,
    WD_OB_EVENT_REQUEST,
    WD_OB_EVENT_META
};

// ============ OneBot 消息结构 ============
struct WDOneBotMessage {
    int64_t user_id;
    int64_t group_id;
    std::string message;
    std::string raw_message;
    std::string message_type;  // "private" 或 "group"
    std::string sub_type;
    int64_t time;
    std::string self_id;
    
    WDOneBotMessage() : user_id(0), group_id(0), time(0) {}
};

// ============ OneBot 客户端 ============
class WDOneBotClient {
private:
    std::unique_ptr<WDWebSocketClient> ws;
    WDOneBotConfig config;
    std::string self_id;
    std::string self_name;
    std::atomic<bool> ready{false};
    std::atomic<bool> initialized{false};
    std::mutex status_mutex;
    std::map<std::string, bool> last_process_status;
    
    // 事件回调
    std::function<void(const WDOneBotMessage&)> on_private_message;
    std::function<void(const WDOneBotMessage&)> on_group_message;
    std::function<void(const json&)> on_notice_event;
    std::function<void(const json&)> on_request_event;
    std::function<void(const json&)> on_meta_event;
    
public:
    WDOneBotClient() : ws(std::make_unique<WDWebSocketClient>()) {}
    ~WDOneBotClient() { disconnect(); }
    
    // 初始化
    bool init(const WDOneBotConfig& cfg) {
        config = cfg;
        if (!config.enabled) {
            WD_LOG_INFO("OneBot 已禁用");
            initialized = true;
            return true;
        }
        
        WDWebSocketCallbacks cb;
        cb.on_open = [this]() {
            WD_LOG_INFO("OneBot WebSocket 已连接");
            this->ready = true;
            // 获取自身信息
            json payload = {
                {"action", "get_login_info"},
                {"params", json::object()},
                {"echo", "get_self_id"}
            };
            this->ws->send(payload);
        };
        
        cb.on_message = [this](const json& msg) {
            this->handle_message(msg);
        };
        
        cb.on_close = [this](int code, const std::string& reason) {
            WD_LOG_WARN("OneBot 连接断开: " << code << " " << reason);
            this->ready = false;
        };
        
        cb.on_error = [](const std::string& error) {
            WD_LOG_ERROR("OneBot WebSocket 错误: " << error);
        };
        
        if (!ws->init(config.ws_url, cb)) {
            WD_LOG_ERROR("OneBot WebSocket 初始化失败");
            return false;
        }
        
        initialized = true;
        return true;
    }
    
    // 连接
    bool connect() {
        if (!config.enabled) return true;
        if (!initialized) return false;
        if (!ws->connect()) return false;
        ws->start();
        return true;
    }
    
    // 断开
    void disconnect() {
        if (!config.enabled) return;
        if (ws->is_running()) {
            ws->stop();
        }
        ready = false;
    }
    
    // ===== 发送消息到所有目标 =====
    bool send_message(const std::string& message) {
        if (!config.enabled || !ready || message.empty()) return false;
        
        bool all_success = true;
        
        // 发送到所有群
        for (int64_t group_id : config.group_ids) {
            if (!send_group_msg(group_id, message)) {
                WD_LOG_WARN("发送群消息失败: " << group_id);
                all_success = false;
            }
        }
        
        // 发送给所有私聊
        for (int64_t user_id : config.user_ids) {
            if (!send_private_msg(user_id, message)) {
                WD_LOG_WARN("发送私聊消息失败: " << user_id);
                all_success = false;
            }
        }
        
        return all_success;
    }
    
    // ===== 使用模板发送消息 =====
    bool send_template(const std::string& template_str, 
                       const std::map<std::string, std::string>& vars) {
        if (!config.enabled || template_str.empty()) return false;
        
        std::string message = template_str;
        for (const auto& [key, value] : vars) {
            std::string placeholder = "{" + key + "}";
            size_t pos = 0;
            while ((pos = message.find(placeholder, pos)) != std::string::npos) {
                message.replace(pos, placeholder.length(), value);
                pos += value.length();
            }
        }
        
        return send_message(message);
    }
    
    // ===== 发送群消息 =====
    bool send_group_msg(int64_t group_id, const std::string& message) {
        if (!ready || group_id == 0) return false;
        
        json payload = {
            {"action", "send_group_msg"},
            {"params", {
                {"group_id", group_id},
                {"message", message}
            }},
            {"echo", "send_group_" + std::to_string(group_id) + "_" + std::to_string(std::time(nullptr))}
        };
        return ws->send(payload);
    }
    
    // ===== 发送私聊消息 =====
    bool send_private_msg(int64_t user_id, const std::string& message) {
        if (!ready || user_id == 0) return false;
        
        json payload = {
            {"action", "send_private_msg"},
            {"params", {
                {"user_id", user_id},
                {"message", message}
            }},
            {"echo", "send_private_" + std::to_string(user_id) + "_" + std::to_string(std::time(nullptr))}
        };
        return ws->send(payload);
    }
    
    // ===== 上报进程状态变化 =====
    void report_process_change(const std::string& name, bool now_running, 
                                int restart_count = 0, WDProcessId pid = WD_INVALID_PROCESS_ID) {
        if (!config.enabled || !config.report_on_process_change) return;
        if (!ready) return;
        
        std::lock_guard<std::mutex> lock(status_mutex);
        auto it = last_process_status.find(name);
        bool last_running = (it != last_process_status.end()) ? it->second : false;
        
        // 只有状态变化时才上报
        if (last_running == now_running) return;
        last_process_status[name] = now_running;
        
        std::map<std::string, std::string> vars = {
            {"name", name},
            {"pid", (pid == WD_INVALID_PROCESS_ID) ? "未知" : std::to_string(pid)},
            {"restart_count", std::to_string(restart_count)},
            {"status", now_running ? "运行中" : "已停止"},
            {"time", wd_get_timestamp()}
        };
        
        std::string template_str;
        if (now_running) {
            template_str = config.process_restart_message.empty() 
                ? "🔄 进程已重启: {name}\n新PID: {pid}\n时间: {time}" 
                : config.process_restart_message;
        } else {
            template_str = config.process_down_message.empty()
                ? "💀 进程崩溃: {name}\nPID: {pid}\n已重启: {restart_count}次\n时间: {time}"
                : config.process_down_message;
        }
        
        send_template(template_str, vars);
    }
    
    // ===== 上报完整状态 =====
    void report_status(const std::vector<WDProcessConfig>& processes) {
        if (!config.enabled || !ready) return;
        
        int running_count = 0;
        std::string process_list;
        
        for (const auto& pc : processes) {
            if (pc.is_running) running_count++;
            std::string status_icon = pc.is_running ? "✅" : "❌";
            std::string pid_str = (pc.pid == WD_INVALID_PROCESS_ID) ? "未启动" : std::to_string(pc.pid);
            process_list += "• " + pc.name + ": " + status_icon + " (PID: " + pid_str + ")\n";
        }
        
        // 获取系统信息
        WDSystemInfo sys_info = wd_get_system_info();
        
        std::map<std::string, std::string> vars = {
            {"running_count", std::to_string(running_count)},
            {"total_count", std::to_string(processes.size())},
            {"process_list", process_list},
            {"self_id", self_id},
            {"hostname", sys_info.hostname},
            {"os", sys_info.os_name},
            {"time", wd_get_timestamp()}
        };
        
        std::string template_str = config.status_message.empty()
            ? "📊 看门狗状态报告\n"
              "主机: {hostname}\n"
              "系统: {os}\n"
              "时间: {time}\n"
              "运行: {running_count}/{total_count}\n"
              "{process_list}"
            : config.status_message;
        
        send_template(template_str, vars);
    }
    
    // ===== 上报启动消息 =====
    void report_start(const std::vector<WDProcessConfig>& processes) {
        if (!config.enabled || !config.report_on_start) return;
        if (!ready) return;
        
        WDSystemInfo sys_info = wd_get_system_info();
        
        std::map<std::string, std::string> vars = {
            {"process_count", std::to_string(processes.size())},
            {"self_id", self_id},
            {"hostname", sys_info.hostname},
            {"os", sys_info.os_name},
            {"time", wd_get_timestamp()},
            {"pid", std::to_string(sys_info.pid)}
        };
        
        std::string template_str = config.start_message.empty()
            ? "🐱 看门狗已启动\n"
              "主机: {hostname}\n"
              "系统: {os}\n"
              "进程数: {process_count}\n"
              "PID: {pid}\n"
              "时间: {time}"
            : config.start_message;
        
        send_template(template_str, vars);
    }
    
    // ===== 上报看门狗自身状态 =====
    void report_heartbeat() {
        if (!config.enabled || !ready) return;
        
        std::map<std::string, std::string> vars = {
            {"time", wd_get_timestamp()},
            {"self_id", self_id},
            {"status", "运行中"}
        };
        
        send_template("❤️ 看门狗心跳\n时间: {time}\n状态: {status}", vars);
    }
    
    // ===== 上报错误 =====
    void report_error(const std::string& error_msg) {
        if (!config.enabled || !ready) return;
        
        std::map<std::string, std::string> vars = {
            {"error", error_msg},
            {"time", wd_get_timestamp()},
            {"self_id", self_id}
        };
        
        send_template("⚠️ 看门狗错误\n错误: {error}\n时间: {time}", vars);
    }
    
    bool is_ready() const { return ready; }
    bool is_enabled() const { return config.enabled; }
    std::string get_self_id() const { return self_id; }
    
    // ===== 设置事件回调 =====
    void set_on_private_message(std::function<void(const WDOneBotMessage&)> cb) { 
        on_private_message = cb; 
    }
    void set_on_group_message(std::function<void(const WDOneBotMessage&)> cb) { 
        on_group_message = cb; 
    }
    void on_notice(std::function<void(const json&)> cb) { 
        on_notice_event = cb; 
    }
    void on_request(std::function<void(const json&)> cb) { 
        on_request_event = cb; 
    }
    void on_meta(std::function<void(const json&)> cb) { 
        on_meta_event = cb; 
    }
    
private:
    // ===== 处理收到的消息 =====
    void handle_message(const json& msg) {
        // 检查是否是响应（echo）
        if (msg.contains("echo")) {
            if (msg["echo"] == "get_self_id" && msg.contains("data")) {
                self_id = msg["data"].value("user_id", "");
                if (!self_id.empty()) {
                    WD_LOG_INFO("OneBot 自身ID: " << self_id);
                }
            }
            return;
        }
        
        // 检查是否是事件
        if (!msg.contains("post_type")) return;
        
        std::string post_type = msg["post_type"];
        
        if (post_type == "message") {
            handle_message_event(msg);
        } else if (post_type == "notice" && on_notice_event) {
            on_notice_event(msg);
        } else if (post_type == "request" && on_request_event) {
            on_request_event(msg);
        } else if (post_type == "meta_event" && on_meta_event) {
            on_meta_event(msg);
        }
    }
    
    // ===== 处理消息事件 =====
    void handle_message_event(const json& msg) {
        WDOneBotMessage message;
        message.time = msg.value("time", 0);
        message.self_id = self_id;
        
        if (msg.contains("message")) {
            message.message = msg["message"];
            message.raw_message = msg["message"];
        }
        
        if (msg.contains("message_type")) {
            message.message_type = msg["message_type"];
        }
        
        if (msg.contains("sub_type")) {
            message.sub_type = msg["sub_type"];
        }
        
        // 私聊消息
        if (message.message_type == "private") {
            if (msg.contains("user_id")) {
                message.user_id = msg["user_id"];
            }
            if (on_private_message) {
                on_private_message(message);
            }
        }
        
        // 群聊消息
        if (message.message_type == "group") {
            if (msg.contains("user_id")) {
                message.user_id = msg["user_id"];
            }
            if (msg.contains("group_id")) {
                message.group_id = msg["group_id"];
            }
            if (on_group_message) {
                on_group_message(message);
            }
        }
        
        // 处理指令（示例：/status 查看状态）
        if (message.message_type == "group" || message.message_type == "private") {
            std::string cmd = message.message;
            if (cmd.find("/status") == 0 || cmd.find("！status") == 0) {
                // 需要由主程序调用 report_status
                // 这里只做示例，实际由主程序处理
                WD_LOG_INFO("收到状态查询指令: " << cmd);
            }
        }
    }
};
