// ================================================================
// watchdog.h - 看门狗完整封装（新版：发送模块已拆分到Python）
// 版本: 2.0
// 说明: 将发送功能拆分为独立Python进程，看门狗通过管道与其通信
//       发送脚本负责所有网络请求（Notifier WS/HTTP + OneBot WS）
// ================================================================

#pragma once

#include "wdsystem.h"
#include "logger.h"
#include "config.h"
#include "cat_sender.h"

#include <functional>
#include <memory>
#include <any>
#include "json.hpp"
#include "process_utils.h"

using json = nlohmann::json;

// 全局运行标志
extern std::atomic<bool> g_running;


// ================================================================
// 1. 初始化与配置
// ================================================================

// 加载配置文件（默认 cw.json）
inline bool wd_init(const std::string& config_path = "cw.json") {
    return WDConfigManager::instance().load(config_path);
}

// 通用配置读取：使用点路径，如 "onebot.enabled"
template<typename T>
inline T wd_get(const std::string& key, const T& default_value = T{}) {
    const auto& cfg = WDConfigManager::instance();
    if (!cfg.is_loaded()) return default_value;

    const json& root = cfg.get_json();
    try {
        std::vector<std::string> parts;
        std::stringstream ss(key);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (!part.empty()) parts.push_back(part);
        }

        json current = root;
        for (const auto& p : parts) {
            if (!current.contains(p)) return default_value;
            current = current[p];
        }
        return current.get<T>();
    } catch (...) {
        return default_value;
    }
}

inline std::string wd_get_str(const std::string& key, const std::string& default_value = "") {
    return wd_get<std::string>(key, default_value);
}

inline int wd_get_int(const std::string& key, int default_value = 0) {
    return wd_get<int>(key, default_value);
}

inline bool wd_get_bool(const std::string& key, bool default_value = false) {
    return wd_get<bool>(key, default_value);
}

inline json wd_get_json(const std::string& key, const json& default_value = json{}) {
    const auto& cfg = WDConfigManager::instance();
    if (!cfg.is_loaded()) return default_value;
    const json& root = cfg.get_json();
    try {
        std::vector<std::string> parts;
        std::stringstream ss(key);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (!part.empty()) parts.push_back(part);
        }
        json current = root;
        for (const auto& p : parts) {
            if (!current.contains(p)) return default_value;
            current = current[p];
        }
        return current;
    } catch (...) {
        return default_value;
    }
}

inline std::vector<WDProcessConfig>& wd_processes() {
    return WDConfigManager::instance().get_all_processes();
}

inline WDProcessConfig* wd_find_process(const std::string& name) {
    return WDConfigManager::instance().get_process_by_name(name);
}

// ================================================================
// 2. 日志
// ================================================================

inline void wd_log_init() {
    auto& cfg = WDConfigManager::instance();
    if (!cfg.is_loaded()) {
        WDLogger::instance().init("logs", WD_LOG_INFO, true, true);
        return;
    }
    std::string dir = wd_get_str("watchdog.log_dir", "logs");
    int level = wd_get_int("watchdog.log_level", WD_LOG_INFO);
    bool console = wd_get_bool("watchdog.console_log", true);
    bool file = wd_get_bool("watchdog.file_log", true);
    WDLogger::instance().init(dir, level, console, file);
}

// ================================================================
// 3. 系统与进程
// ================================================================

inline bool wd_running() {
    return g_running;
}

inline WDSystemInfo wd_sysinfo() {
    return wd_get_system_info();
}

inline void wd_sleep(int seconds) {
    wd_sleep_sec(seconds);
}
inline void wd_msleep(int milliseconds) {
    wd_sleep_ms(milliseconds);
}

inline std::string wd_timestamp() {
    return wd_get_timestamp();
}

inline WDProcessId wd_start(
    const std::string& cmd,
    const std::vector<std::string>& args = {},
    const std::string& workdir = "",
    bool wait = false,
    int timeout_ms = -1,
    bool show_window = true
) {
    WDProcessId pid;
    WDProcessHandle handle;
    if (!wd_start_process(cmd, args, pid, handle, workdir, show_window)) {
        return WD_INVALID_PROCESS_ID;
    }
    if (wait) {
        wd_wait_for_process(handle, timeout_ms);
    }
    return pid;
}

inline bool wd_stop(WDProcessId pid) {
    return wd_kill_process_by_pid(pid);
}

inline bool wd_is_alive(WDProcessId pid) {
    return wd_is_process_alive(pid);
}

// ================================================================
// 4. 发送器接口（通过管道转发给Python）
// ================================================================

// 初始化发送器（从JSON配置）
inline bool wd_sender_init(const std::string& config_path = "cw.json") {
    const auto& cfg = WDConfigManager::instance();
    json sender_cfg = cfg.get_json().value("sender", json::object());
    
    // 自动补全Python路径（尝试找系统中的Python）
    if (!sender_cfg.contains("python_path")) {
        sender_cfg["python_path"] = "python";  // 默认用PATH里的python
    }
    
    // 自动补全脚本路径
    if (!sender_cfg.contains("sender_script")) {
        // 从config_path推导目录
        size_t pos = config_path.find_last_of("/\\");
        std::string dir = (pos != std::string::npos) ? config_path.substr(0, pos + 1) : "";
        sender_cfg["sender_script"] = dir + "sender.py";
    }
    
    if (!sender_cfg.contains("check_interval_sec")) {
        sender_cfg["check_interval_sec"] = 60;
    }
    
    return get_sender().init_from_json(sender_cfg);
}

// 启动发送器
inline bool wd_sender_start() {
    return get_sender().start();
}

// 停止发送器
inline void wd_sender_stop() {
    get_sender().stop();
}

// 通知崩溃
inline void wd_notify_crash(const std::string& name, WDProcessId pid, 
                              int restart_count, const std::string& reason) {
    get_sender().send_crash(name, pid, restart_count, reason);
}

// 通知半启动
inline void wd_notify_half_start(const std::string& name, 
                                   const std::vector<int>& ports,
                                   const std::vector<bool>& port_status) {
    get_sender().send_half_start(name, ports, port_status);
}

// 通知看门狗启动
inline void wd_notify_start() {
    get_sender().send_watchdog_start();
}

// 通知进程变化
inline void wd_notify_process_change(const std::string& name, bool now_running,
                                        int restart_count, WDProcessId pid) {
    get_sender().send_process_change(name, now_running, restart_count, pid);
}

// 通知手动重启
inline void wd_notify_manual_restart(const std::string& name, const std::string& reason) {
    get_sender().send_custom_text(
        "🔄 手动重启: " + name + "\n原因: " + reason + "\n时间: " + wd_timestamp()
    );
}

// 发送自定义文本
inline void wd_notify_text(const std::string& text) {
    get_sender().send_custom_text(text);
}

// ================================================================
// 5. 核心监控功能
// ================================================================

inline bool wd_monitor_once(WDProcessConfig& pc) {
    if (!pc.is_running) {
        if (wd_start(pc.cmd, pc.args, pc.working_dir, false, -1, pc.show_window) != WD_INVALID_PROCESS_ID) {
            pc.is_running = true;
            pc.restart_count = 0;
            WD_LOG_INFO("启动成功: " << pc.name);
            return true;
        }
        return false;
    }

    if (!wd_is_alive(pc.pid)) {
        std::string reason = wd_get_exit_reason(pc.pid, pc.handle);
        WD_LOG_WARN("进程崩溃: " << pc.name << " (PID=" << pc.pid << "), 原因: " << reason);
        pc.is_running = false;

        if (pc.restart && (pc.max_restart == 0 || pc.restart_count < pc.max_restart)) {
            pc.restart_count++;
            int delay = (pc.restart_delay > 0) ? pc.restart_delay : wd_get_int("watchdog.restart_delay_sec", 3);
            wd_sleep(delay);

            if (!pc.cmd.empty()) {
                WD_LOG_INFO("精确结束残留进程: " << pc.cmd);
                wd_kill_process_by_cmdline(pc.cmd);
                wd_kill_process_by_path(pc.cmd);
            }
            wd_sleep_sec(2);

            if (wd_start(pc.cmd, pc.args, pc.working_dir, false, -1, pc.show_window) != WD_INVALID_PROCESS_ID) {
                pc.is_running = true;
                WD_LOG_INFO("重启成功: " << pc.name << " (PID=" << pc.pid << ")");
                return true;
            } else {
                WD_LOG_ERROR("重启失败: " << pc.name);
                return false;
            }
        }
        return false;
    }
    return true;
}

inline void wd_monitor_all(std::vector<WDProcessConfig>& processes) {
    for (auto& pc : processes) {
        wd_monitor_once(pc);
    }
}

inline std::string wd_status_text(const std::vector<WDProcessConfig>& processes) {
    int running = 0;
    std::string lines;
    for (const auto& pc : processes) {
        if (pc.is_running) running++;
        lines += "• " + pc.name + ": " + (pc.is_running ? "✅" : "❌") + "\n";
    }
    return "运行: " + std::to_string(running) + "/" + std::to_string(processes.size()) + "\n" + lines;
}

// ================================================================
// 6. 主循环
// ================================================================

inline void wd_run(
    std::vector<WDProcessConfig>& processes,
    std::function<void()> on_tick = nullptr,
    std::function<void()> on_stop = nullptr
) {
    int interval = wd_get_int("watchdog.feed_interval_sec", 5);
    int timeout = wd_get_int("watchdog.timeout_sec", 15) * 1000;

    int loop_count = 0;

    // 喵云崽特殊处理
    std::map<std::string, std::vector<bool>> last_port_status;
    std::map<std::string, bool> half_start_notified;

    while (g_running) {
        auto start = std::chrono::steady_clock::now();

        for (auto& pc : processes) {
            bool was_running = pc.is_running;
            bool now_running = wd_monitor_once(pc);

            // 崩溃检测
            if (was_running && !now_running) {
                std::string crash_reason = wd_get_exit_reason(pc.pid, pc.handle);
                if (crash_reason == "正常退出" || crash_reason.find("仍在运行") != std::string::npos) {
                    crash_reason = "被结束或未知原因";
                }
                // 通过发送器通报
                wd_notify_crash(pc.name, pc.pid, pc.restart_count, crash_reason);
            }

            if (was_running != now_running) {
                wd_notify_process_change(pc.name, now_running, pc.restart_count, pc.pid);
            }

            // 端口检测
            if (!pc.ports.empty() && now_running) {
                std::vector<bool> current_status;
                int listening_count = 0;
                for (int port : pc.ports) {
                    bool listening = wd_is_port_listening(port);
                    current_status.push_back(listening);
                    if (listening) listening_count++;
                }

                if (pc.ports.size() > 1) {
                    bool is_half_start = (listening_count > 0 && listening_count < (int)pc.ports.size());

                    if (is_half_start) {
                        bool already_notified = half_start_notified[pc.name];

                        if (!already_notified) {
                            wd_notify_half_start(pc.name, pc.ports, current_status);
                            half_start_notified[pc.name] = true;

                            // TODO: 如果有重启请求记录，执行手动重启
                            // 这个逻辑之前是在 notifier 里处理 WS 消息的，现在需要另外的方式
                            // 暂时保留，等你确认需求
                        }
                    } else {
                        half_start_notified[pc.name] = false;
                    }
                }

                last_port_status[pc.name] = current_status;
            }
        }

        // 定时上报完整状态（每30秒）
        loop_count++;
        if (loop_count % (30 / interval) == 0) {
            WDSystemInfo sys = wd_get_system_info();
            get_sender().send_status_report(processes, sys.hostname);
        }

        if (on_tick) on_tick();

        auto end = std::chrono::steady_clock::now();
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        if (dur > timeout) {
            WD_LOG_WARN("主循环超时: " << dur << "ms");
        }

        wd_sleep(interval);
    }

    // 停止前上报
    if (on_stop) on_stop();
}

// ================================================================
// 7. 一键启动
// ================================================================

inline void wd_run_all(const std::string& config_path = "cw.json") {
    // 1. 加载配置
    if (!wd_init(config_path)) {
        WD_LOG_ERROR("配置加载失败，退出");
        return;
    }

    // 2. 初始化日志
    wd_log_init();
    WD_LOG_INFO("看门狗启动，PID=" << wd_get_my_pid());

    // 3. 启动发送器
    if (wd_sender_init(config_path)) {
        if (wd_sender_start()) {
            wd_notify_start();
            WD_LOG_INFO("发送器已启动");
        } else {
            WD_LOG_WARN("发送器启动失败，将只进行本地监控");
        }
    } else {
        WD_LOG_WARN("发送器初始化失败，将只进行本地监控");
    }

    // 4. 启动所有进程
    for (auto& pc : wd_processes()) {
        if (wd_start(pc.cmd, pc.args, pc.working_dir, false, -1, pc.show_window) != WD_INVALID_PROCESS_ID) {
            pc.is_running = true;
            WD_LOG_INFO("启动进程: " << pc.name);
        }
    }

    // 5. 运行主循环
    wd_run(wd_processes());

    // 6. 清理
    for (auto& pc : wd_processes()) {
        if (pc.is_running) wd_stop(pc.pid);
    }
    wd_sender_stop();
    WD_LOG_INFO("看门狗已停止");
}

// ================================================================
// 结束
// ================================================================
