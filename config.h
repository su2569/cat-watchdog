#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include "json.hpp"
#include <fstream>
#include <iostream>
#include "wdsystem.h"
#include "logger.h"

using json = nlohmann::json;

// ============ 进程配置结构 ============
struct WDProcessConfig {
    std::string name;                    // 进程名称
    std::string cmd;                     // 启动命令
    std::vector<std::string> args;       // 命令行参数
    std::string working_dir;             // 工作目录（可选）
    std::vector<std::string> keywords;   // 关键词（用于监控匹配）
    std::vector<int> ports;              // 监听端口（备用监控）
    bool restart;                        // 是否自动重启
    int max_restart;                     // 最大重启次数（0=无限）
    int restart_delay;                   // 重启延迟（秒）
    
    // 运行时状态
    int restart_count;
    WDProcessId pid;
    WDProcessHandle handle;
    bool is_running;
    
    bool show_window;  // Windows: 是否显示控制台窗口

    WDProcessConfig() : restart(true), max_restart(5), restart_delay(3),
                        restart_count(0), pid(WD_INVALID_PROCESS_ID), 
                        handle(WD_INVALID_HANDLE), is_running(false),
                        show_window(true) {}
};

// ============ OneBot 配置 ============
struct WDOneBotConfig {
    bool enabled;
    std::string ws_url;
    std::vector<int64_t> group_ids;      // 群聊列表
    std::vector<int64_t> user_ids;       // 私聊列表
    int report_interval_sec;
    bool report_on_start;
    bool report_on_process_change;
    
    // 消息模板
    std::string start_message;
    std::string status_message;
    std::string process_down_message;
    std::string process_restart_message;
    
    WDOneBotConfig() : enabled(false), ws_url("ws://127.0.0.1:6700"),
                       report_interval_sec(30), report_on_start(true),
                       report_on_process_change(true) {}
};

// ============ 看门狗全局配置 ============
struct WDWatchdogConfig {
    int feed_interval_sec;
    int timeout_sec;
    std::string log_dir;
    int max_restart_count;
    int restart_delay_sec;
    int log_level;
    bool console_log;
    bool file_log;
    
    WDOneBotConfig onebot;
    std::vector<WDProcessConfig> processes;
    
    WDWatchdogConfig() : feed_interval_sec(5), timeout_sec(15),
                         log_dir("logs"), max_restart_count(10), 
                         restart_delay_sec(3), log_level(WD_LOG_INFO), 
                         console_log(true), file_log(true) {}
};

// ============ 配置管理器（单例） ============
class WDConfigManager {
private:
    WDWatchdogConfig config;
    json root_json;
    std::string config_path;
    bool loaded;
    mutable std::mutex mtx;
    
    WDConfigManager() : loaded(false) {}
    
public:
    static WDConfigManager& instance() {
        static WDConfigManager manager;
        return manager;
    }
    
    // ===== 加载配置 =====
    bool load(const std::string& path = "cw.json") {
        std::lock_guard<std::mutex> lock(mtx);
        config_path = path;
        loaded = false;
        
        std::ifstream file(path);
        if (!file.is_open()) {
            WD_LOG_ERROR("无法打开配置文件: " << path);
            return false;
        }
        
        json root;
        root_json = json();
        try {
            file >> root;
        root_json = root;
        } catch (const json::parse_error& e) {
            WD_LOG_ERROR("JSON解析失败: " << e.what());
            return false;
        }
        
        // 加载看门狗配置
        if (root.contains("watchdog")) {
            auto& wd = root["watchdog"];
            config.feed_interval_sec = wd.value("feed_interval_sec", 5);
            config.timeout_sec = wd.value("timeout_sec", 15);
            config.log_dir = wd.value("log_dir", "logs");
            config.max_restart_count = wd.value("max_restart_count", 10);
            config.restart_delay_sec = wd.value("restart_delay_sec", 3);
            config.log_level = wd.value("log_level", WD_LOG_INFO);
            config.console_log = wd.value("console_log", true);
            config.file_log = wd.value("file_log", true);
        }
        
        // 加载 OneBot 配置
        if (root.contains("onebot")) {
            auto& ob = root["onebot"];
            config.onebot.enabled = ob.value("enabled", false);
            config.onebot.ws_url = ob.value("ws_url", "ws://127.0.0.1:6700");
            
            // 加载群列表
            if (ob.contains("group_ids")) {
                if (ob["group_ids"].is_array()) {
                    config.onebot.group_ids = ob["group_ids"].get<std::vector<int64_t>>();
                } else if (ob["group_ids"].is_number()) {
                    config.onebot.group_ids.push_back(ob["group_ids"].get<int64_t>());
                }
            } else if (ob.contains("group_id")) {
                config.onebot.group_ids.push_back(ob["group_id"].get<int64_t>());
            }
            
            // 加载私聊列表
            if (ob.contains("user_ids")) {
                if (ob["user_ids"].is_array()) {
                    config.onebot.user_ids = ob["user_ids"].get<std::vector<int64_t>>();
                } else if (ob["user_ids"].is_number()) {
                    config.onebot.user_ids.push_back(ob["user_ids"].get<int64_t>());
                }
            } else if (ob.contains("user_id")) {
                config.onebot.user_ids.push_back(ob["user_id"].get<int64_t>());
            }
            
            config.onebot.report_interval_sec = ob.value("report_interval_sec", 30);
            config.onebot.report_on_start = ob.value("report_on_start", true);
            config.onebot.report_on_process_change = ob.value("report_on_process_change", true);
            
            config.onebot.start_message = ob.value("start_message", "");
            config.onebot.status_message = ob.value("status_message", "");
            config.onebot.process_down_message = ob.value("process_down_message", "");
            config.onebot.process_restart_message = ob.value("process_restart_message", "");
        }
        
        // 加载进程列表
        if (root.contains("processes") && root["processes"].is_array()) {
            for (const auto& item : root["processes"]) {
                WDProcessConfig pc;
                pc.name = item.value("name", "unknown");
                pc.cmd = item.value("cmd", "");
                pc.args = item.value("args", std::vector<std::string>());
                pc.working_dir = item.value("working_dir", "");
                pc.keywords = item.value("keywords", std::vector<std::string>());
                pc.ports = item.value("ports", std::vector<int>());
                pc.restart = item.value("restart", true);
                pc.max_restart = item.value("max_restart", 5);
                pc.restart_delay = item.value("restart_delay", 3);
                pc.show_window = item.value("show_window", true);

                // 加载端口列表
                if (item.contains("ports") && item["ports"].is_array()) {
                    for (const auto& p : item["ports"]) {
                        if (p.is_number()) {
                            pc.ports.push_back(p.get<int>());
                        }
                    }
                }
                
                // 自动补全工作目录
                if (pc.working_dir.empty()) {
                    size_t last_sep = pc.cmd.find_last_of(WD_PATH_SEP);
                    if (last_sep != std::string::npos) {
                        pc.working_dir = pc.cmd.substr(0, last_sep);
                    } else {
                        pc.working_dir = ".";
                    }
                }
                
                config.processes.push_back(pc);
            }
        }
        
        loaded = true;
        WD_LOG_INFO("配置加载成功: " << config.processes.size() << " 个进程");
        return true;
    }
    
    // ===== 获取全局配置 =====
    WDWatchdogConfig get_config() {
        std::lock_guard<std::mutex> lock(mtx);
        return config;
    }
    
    // ===== 按名称获取进程配置 =====
    WDProcessConfig* get_process_by_name(const std::string& name) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& pc : config.processes) {
            if (pc.name == name) {
                return &pc;
            }
        }
        return nullptr;
    }
    
    // ===== 按索引获取进程配置 =====
    WDProcessConfig* get_process_by_index(size_t index) {
        std::lock_guard<std::mutex> lock(mtx);
        if (index < config.processes.size()) {
            return &config.processes[index];
        }
        return nullptr;
    }
    
    // ===== 获取所有进程配置 =====
    std::vector<WDProcessConfig>& get_all_processes() {
        std::lock_guard<std::mutex> lock(mtx);
        return config.processes;
    }
    
    // ===== 按关键词查找进程 =====
    std::vector<int> find_processes_by_keyword(const std::string& keyword) {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<int> result;
        for (size_t i = 0; i < config.processes.size(); ++i) {
            for (const auto& kw : config.processes[i].keywords) {
                if (kw == keyword) {
                    result.push_back(static_cast<int>(i));
                    break;
                }
            }
        }
        return result;
    }
    
    // ===== 按端口查找进程 =====
    int find_process_by_port(int port) {
        std::lock_guard<std::mutex> lock(mtx);
        for (size_t i = 0; i < config.processes.size(); ++i) {
            for (int p : config.processes[i].ports) {
                if (p == port) {
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    }
    
    // ===== 获取进程数量 =====
    size_t get_process_count() {
        std::lock_guard<std::mutex> lock(mtx);
        return config.processes.size();
    }
    
    // ===== 获取 OneBot 配置 =====
    WDOneBotConfig get_onebot_config() {
        std::lock_guard<std::mutex> lock(mtx);
        return config.onebot;
    }
    
    // ===== 获取日志配置 =====
    std::string get_log_dir() {
        std::lock_guard<std::mutex> lock(mtx);
        return config.log_dir;
    }
    
    int get_log_level() {
        std::lock_guard<std::mutex> lock(mtx);
        return config.log_level;
    }
    
    bool get_console_log() {
        std::lock_guard<std::mutex> lock(mtx);
        return config.console_log;
    }
    
    bool get_file_log() {
        std::lock_guard<std::mutex> lock(mtx);
        return config.file_log;
    }
    
    // ===== 更新进程运行状态 =====
    void update_process_status(const std::string& name, bool running, 
                               WDProcessId pid = WD_INVALID_PROCESS_ID, 
                               WDProcessHandle handle = WD_INVALID_HANDLE) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& pc : config.processes) {
            if (pc.name == name) {
                pc.is_running = running;
                if (pid != WD_INVALID_PROCESS_ID) pc.pid = pid;
                if (handle != WD_INVALID_HANDLE) pc.handle = handle;
                break;
            }
        }
    }
    
    // ===== 增加重启计数 =====
    void increment_restart_count(const std::string& name) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& pc : config.processes) {
            if (pc.name == name) {
                pc.restart_count++;
                break;
            }
        }
    }
    
    // ===== 重置重启计数 =====
    void reset_restart_count(const std::string& name) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& pc : config.processes) {
            if (pc.name == name) {
                pc.restart_count = 0;
                break;
            }
        }
    }
    
    // ===== 打印配置（调试） =====
    void dump() {
        std::lock_guard<std::mutex> lock(mtx);
        WD_LOG_INFO("========== 看门狗配置 ==========");
        WD_LOG_INFO("喂狗间隔: " << config.feed_interval_sec << "s");
        WD_LOG_INFO("超时阈值: " << config.timeout_sec << "s");
        WD_LOG_INFO("日志目录: " << config.log_dir);
        WD_LOG_INFO("最大重启次数: " << config.max_restart_count);
        WD_LOG_INFO("重启延迟: " << config.restart_delay_sec << "s");
        WD_LOG_INFO("日志级别: " << config.log_level);
        WD_LOG_INFO("控制台日志: " << (config.console_log ? "开" : "关"));
        
        if (config.onebot.enabled) {
            WD_LOG_INFO("OneBot: 启用");
            WD_LOG_INFO("  WS地址: " << config.onebot.ws_url);
            WD_LOG_INFO("  群ID数: " << config.onebot.group_ids.size());
            WD_LOG_INFO("  私聊数: " << config.onebot.user_ids.size());
            WD_LOG_INFO("  上报间隔: " << config.onebot.report_interval_sec << "s");
        } else {
            WD_LOG_INFO("OneBot: 禁用");
        }
        
        WD_LOG_INFO("--------------------------------");
        for (const auto& pc : config.processes) {
            WD_LOG_INFO("进程: " << pc.name);
            WD_LOG_INFO("  命令: " << pc.cmd);
            WD_LOG_INFO("  参数: " << (pc.args.empty() ? "无" : pc.args[0]));
            for (size_t i = 1; i < pc.args.size(); ++i) {
                WD_LOG_INFO("        " << pc.args[i]);
            }
            WD_LOG_INFO("  工作目录: " << pc.working_dir);
            WD_LOG_INFO("  关键词: " << (pc.keywords.empty() ? "无" : pc.keywords[0]));
            WD_LOG_INFO("  端口: " << (pc.ports.empty() ? "无" : std::to_string(pc.ports[0])));
            WD_LOG_INFO("  自动重启: " << (pc.restart ? "是" : "否"));
            WD_LOG_INFO("  最大重启: " << (pc.max_restart == 0 ? "无限" : std::to_string(pc.max_restart)));
            WD_LOG_INFO("  当前PID: " << (pc.pid == WD_INVALID_PROCESS_ID ? "未启动" : std::to_string(pc.pid)));
            WD_LOG_INFO("  运行状态: " << (pc.is_running ? "运行中" : "已停止"));
            WD_LOG_INFO("--------------------------------");
        }
        WD_LOG_INFO("==================================");
    }
    
    bool is_loaded() const {
        std::lock_guard<std::mutex> lock(mtx);
        return loaded;
    }
    
    json get_json() const {
        std::lock_guard<std::mutex> lock(mtx);
        return root_json;
    }
};

// ============ 便捷宏 ============
#define WD_CFG WDConfigManager::instance()
#define WD_GET_CONFIG() WD_CFG.get_config()
#define WD_GET_PROCESS(name) WD_CFG.get_process_by_name(name)
#define WD_GET_ALL_PROCESSES() WD_CFG.get_all_processes()
#define WD_GET_ONEBOT() WD_CFG.get_onebot_config()
