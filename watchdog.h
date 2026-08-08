// ================================================================
// watchdog.h - 看门狗完整封装
// 版本: 1.0
// 说明: 整合所有功能，提供最简 API
// ================================================================

#pragma once

#include "wdsystem.h"
#include "logger.h"
#include "config.h"
#include "onebot_client.h"
#include "http_reporter.h"

#include <functional>
#include <memory>
#include <any>
#include "json.hpp"
#include "notification.h"
#include "process_utils.h"

using json = nlohmann::json;

// 全局运行标志
extern std::atomic<bool> g_running;

// 全局通知器（定义在 notification.h 中）
extern WDNotifier g_notifier;


// ================================================================
// 1. 初始化与配置
// ================================================================

// 加载配置文件（默认 cw.json）
inline bool wd_init(const std::string& config_path = "cw.json") {
    return WDConfigManager::instance().load(config_path);
}

// 通用配置读取：使用点路径，如 "onebot.enabled"
// 如果键不存在，返回默认值
template<typename T>
inline T wd_get(const std::string& key, const T& default_value = T{}) {
    const auto& cfg = WDConfigManager::instance();
    if (!cfg.is_loaded()) return default_value;

    // 获取原始 JSON
    const json& root = cfg.get_json();
    try {
        // 分割路径
        std::vector<std::string> parts;
        std::stringstream ss(key);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (!part.empty()) parts.push_back(part);
        }

        // 逐级访问
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

// 重载：返回 std::string（默认空）
inline std::string wd_get_str(const std::string& key, const std::string& default_value = "") {
    return wd_get<std::string>(key, default_value);
}

// 重载：返回 int
inline int wd_get_int(const std::string& key, int default_value = 0) {
    return wd_get<int>(key, default_value);
}

// 重载：返回 bool
inline bool wd_get_bool(const std::string& key, bool default_value = false) {
    return wd_get<bool>(key, default_value);
}

// 重载：返回 json（用于复杂对象）
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

// 获取进程配置列表（直接引用，用于高性能访问）
inline std::vector<WDProcessConfig>& wd_processes() {
    return WDConfigManager::instance().get_all_processes();
}

// 按名称查找进程配置（返回指针）
inline WDProcessConfig* wd_find_process(const std::string& name) {
    return WDConfigManager::instance().get_process_by_name(name);
}

// ================================================================
// 2. 日志
// ================================================================

// 初始化日志（自动从配置读取 log_dir 和 log_level）
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

// 日志宏在 logger.h 中定义

// ================================================================
// 3. 系统与进程
// ================================================================

// 注册信号
inline void wd_signal_setup() {
    wd_signal_setup();
}

// 是否运行中（全局标志）
inline bool wd_running() {
    return g_running;
}

// 获取系统信息
inline WDSystemInfo wd_sysinfo() {
    return wd_get_system_info();
}

// 睡眠
inline void wd_sleep(int seconds) {
    wd_sleep_sec(seconds);
}
inline void wd_msleep(int milliseconds) {
    wd_sleep_ms(milliseconds);
}

// 时间戳
inline std::string wd_timestamp() {
    return wd_get_timestamp();
}

// 启动进程（简易版）
// 返回 PID，失败返回 WD_INVALID_PROCESS_ID
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

// 停止进程（通过 PID）
inline bool wd_stop(WDProcessId pid) {
    return wd_kill_process_by_pid(pid);
}

// 检查进程是否存活
inline bool wd_is_alive(WDProcessId pid) {
    return wd_is_process_alive(pid);
}

// 执行命令并获取输出（仅 Linux）
inline std::string wd_exec(const std::string& cmd) {
#ifdef _WIN32
    return "";
#else
    std::string result;
    char buffer[128];
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
#endif
}

// 获取环境变量
inline std::string wd_env(const std::string& name) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : "";
}

// ================================================================
// 4. OneBot 客户端（自动从配置创建）
// ================================================================

// 创建并连接 OneBot（如果启用）
inline std::unique_ptr<WDOneBotClient> wd_onebot_create() {
    auto cfg = WDConfigManager::instance().get_onebot_config();
    if (!cfg.enabled) return nullptr;
    auto client = std::make_unique<WDOneBotClient>();
    if (!client->init(cfg) || !client->connect()) {
        WD_LOG_ERROR("OneBot 连接失败");
        return nullptr;
    }
    return client;
}

// 发送消息（如果客户端已就绪）
inline bool wd_onebot_send(WDOneBotClient* client, const std::string& msg) {
    if (!client || !client->is_ready()) return false;
    return client->send_message(msg);
}

// 上报状态（OneBot）
inline bool wd_onebot_report_status(WDOneBotClient* client, const std::vector<WDProcessConfig>& processes) {
    if (!client || !client->is_ready()) return false;
    client->report_status(processes);
    return true;
}

// ================================================================
// 5. HTTP 上报器（自动从配置创建）
// ================================================================

// 从配置创建 HTTP 上报器（需要在 cw.json 中有 http_reporter 段）
inline std::unique_ptr<WDHttpReporter> wd_http_create() {
    // 使用通用配置读取
    bool enabled = wd_get_bool("http_reporter.enabled", false);
    if (!enabled) return nullptr;

    WDHttpReporterConfig cfg;
    cfg.enabled = true;
    cfg.url = wd_get_str("http_reporter.url", "");
    cfg.method = wd_get_str("http_reporter.method", "POST");
    // headers 为复杂对象，需要单独读取
    json headers_json = wd_get_json("http_reporter.headers", json::object());
    for (auto& [k, v] : headers_json.items()) {
        if (v.is_string()) cfg.headers[k] = v.get<std::string>();
    }
    cfg.timeout_sec = wd_get_int("http_reporter.timeout_sec", 10);
    cfg.retry_count = wd_get_int("http_reporter.retry_count", 3);
    cfg.retry_delay_sec = wd_get_int("http_reporter.retry_delay_sec", 5);
    cfg.report_on_start = wd_get_bool("http_reporter.report_on_start", true);
    cfg.report_on_change = wd_get_bool("http_reporter.report_on_change", true);
    cfg.report_interval_sec = wd_get_int("http_reporter.report_interval_sec", 60);

    auto reporter = std::make_unique<WDHttpReporter>();
    if (!reporter->init(cfg)) {
        WD_LOG_ERROR("HTTP 上报器初始化失败");
        return nullptr;
    }
    reporter->start();
    return reporter;
}

// 上报状态（HTTP）
inline bool wd_http_report_status(WDHttpReporter* reporter, const std::vector<WDProcessConfig>& processes) {
    if (!reporter || !reporter->is_running()) return false;
    return reporter->report_status(processes);
}

// ================================================================
// 6. 核心监控功能
// ================================================================

// 监控单个进程（自动重启），返回 true 表示进程当前运行
inline bool wd_monitor_once(WDProcessConfig& pc) {
    if (!pc.is_running) {
        // 尝试启动
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

            // 使用精确匹配结束残留进程（避免误杀其他 python/node 进程）
            if (!pc.cmd.empty()) {
                WD_LOG_INFO("精确结束残留进程: " << pc.cmd);
                wd_kill_process_by_cmdline(pc.cmd);
                // 也尝试通过路径匹配
                wd_kill_process_by_path(pc.cmd);
            }
            // 等待残留进程退出
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

// 监控所有进程
inline void wd_monitor_all(std::vector<WDProcessConfig>& processes) {
    for (auto& pc : processes) {
        wd_monitor_once(pc);
    }
}

// 生成状态文本（用于上报）
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
// 7. 主循环（完整封装）
// ================================================================

// 主循环运行器
// 参数:
//   processes   - 进程配置列表（引用）
//   ob          - OneBot 客户端（可选）
//   reporter    - HTTP 上报器（可选）
//   on_tick     - 每次循环回调（可用于自定义逻辑）
//   on_stop     - 停止时回调
inline void wd_run(
    std::vector<WDProcessConfig>& processes,
    WDOneBotClient* ob = nullptr,
    WDHttpReporter* reporter = nullptr,
    std::function<void()> on_tick = nullptr,
    std::function<void()> on_stop = nullptr
) {
    int interval = wd_get_int("watchdog.feed_interval_sec", 5);
    int timeout = wd_get_int("watchdog.timeout_sec", 15) * 1000;
    int report_interval = 0;
    if (ob) report_interval = wd_get_int("onebot.report_interval_sec", 30);
    else if (reporter) report_interval = wd_get_int("http_reporter.report_interval_sec", 60);

    int loop_count = 0;

    // 喵云崽特殊处理：记录上一次端口状态
    std::map<std::string, std::vector<bool>> last_port_status;
    std::map<std::string, bool> half_start_notified;

    while (g_running) {
        auto start = std::chrono::steady_clock::now();

        // 监控所有进程
        for (auto& pc : processes) {
            bool was_running = pc.is_running;
            bool now_running = wd_monitor_once(pc);

            // 崩溃检测：从运行变为不运行
            if (was_running && !now_running) {
                // 获取崩溃原因
                std::string crash_reason = wd_get_exit_reason(pc.pid, pc.handle);
                if (crash_reason == "正常退出" || crash_reason.find("仍在运行") != std::string::npos) {
                    crash_reason = "被结束或未知原因";
                }
                // 通报并重启
                g_notifier.notify_crash(pc.name, pc.pid, pc.restart_count, crash_reason);
            }

            if (was_running != now_running) {
                // 状态变化，上报 OneBot/HTTP
                if (ob && ob->is_ready()) {
                    ob->report_process_change(pc.name, now_running, pc.restart_count, pc.pid);
                }
                if (reporter && reporter->is_running()) {
                    reporter->report_process_change(pc.name, now_running, pc.restart_count, pc.pid);
                }
            }

            // 端口检测（如果配置了 ports）
            if (!pc.ports.empty() && now_running) {
                std::vector<bool> current_status;
                int listening_count = 0;
                for (int port : pc.ports) {
                    bool listening = wd_is_port_listening(port);
                    current_status.push_back(listening);
                    if (listening) listening_count++;
                }

                // 喵云崽半启动检测：多个端口但部分未监听
                if (pc.ports.size() > 1) {
                    bool is_half_start = (listening_count > 0 && listening_count < (int)pc.ports.size());

                    if (is_half_start) {
                        // 检查是否已经通知过（避免重复通知）
                        bool already_notified = half_start_notified[pc.name];

                        if (!already_notified) {
                            g_notifier.notify_half_start(pc.name, pc.ports, current_status);
                            half_start_notified[pc.name] = true;

                            // 如果有重启请求记录，执行手动重启
                            if (g_notifier.has_restart_request()) {
                                WD_LOG_WARN("喵云崽半启动 + 有重启记录，执行手动重启");
                                g_notifier.notify_manual_restart(pc.name, "半启动状态 + 用户重启请求");

                                // 使用精确匹配结束进程树（避免误杀其他 python/node 进程）
                                if (!pc.cmd.empty()) {
                                    WD_LOG_INFO("精确结束进程树: " << pc.cmd);
                                    // 先尝试通过命令行匹配
                                    wd_kill_process_by_cmdline(pc.cmd);
                                    // 再尝试通过路径匹配
                                    wd_kill_process_by_path(pc.cmd);
                                    // 如果知道 PID，也结束进程树
                                    if (pc.pid != WD_INVALID_PROCESS_ID) {
                                        wd_kill_process_tree(pc.pid);
                                    }
                                }

                                // 等待进程完全退出
                                wd_sleep_sec(3);

                                // 检查端口是否还占用，如果还占用则通过端口查找进程并结束
                                for (int port : pc.ports) {
                                    if (wd_is_port_listening(port)) {
                                        WD_LOG_WARN("端口 " << port << " 仍被占用");
                                    }
                                }

                                // 重置状态并重新启动
                                pc.is_running = false;
                                pc.pid = WD_INVALID_PROCESS_ID;
                                pc.handle = WD_INVALID_HANDLE;
                                pc.restart_count = 0;
                                half_start_notified[pc.name] = false;

                                // 重新启动
                                WDProcessId new_pid;
                                WDProcessHandle new_handle;
                                if (wd_start_process(pc.cmd, pc.args, new_pid, new_handle,
                                                     pc.working_dir, pc.show_window)) {
                                    pc.pid = new_pid;
                                    pc.handle = new_handle;
                                    pc.is_running = true;
                                    pc.restart_count = 0;
                                    WD_LOG_INFO("手动重启成功: " << pc.name << " (PID=" << new_pid << ")");
                                } else {
                                    WD_LOG_ERROR("手动重启失败: " << pc.name);
                                }

                                // 清除重启请求记录
                                g_notifier.clear_restart_request();
                            }
                        }
                    } else {
                        // 恢复正常，重置通知标记
                        half_start_notified[pc.name] = false;
                    }
                }

                last_port_status[pc.name] = current_status;
            }
        }

        // 定时上报完整状态
        loop_count++;
        if (report_interval > 0 && (loop_count % (report_interval / interval) == 0)) {
            if (ob && ob->is_ready()) ob->report_status(processes);
            if (reporter && reporter->is_running()) reporter->report_status(processes);
        }

        // 自定义回调
        if (on_tick) on_tick();

        // 检查循环耗时
        auto end = std::chrono::steady_clock::now();
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        if (dur > timeout) {
            WD_LOG_WARN("主循环超时: " << dur << "ms");
        }

        wd_sleep(interval);
    }

    // 停止前上报最终状态
    if (ob && ob->is_ready()) ob->report_status(processes);
    if (reporter && reporter->is_running()) reporter->report_status(processes);
    if (on_stop) on_stop();
}

// ================================================================
// 8. 便捷启动函数（一键启动所有）
// ================================================================

// 一键启动看门狗（加载配置、初始化日志、连接 OneBot/HTTP、进入主循环）
inline void wd_run_all(const std::string& config_path = "cw.json") {
    // 1. 信号
    wd_signal_setup();

    // 2. 加载配置
    if (!wd_init(config_path)) {
        WD_LOG_ERROR("配置加载失败，退出");
        return;
    }

    // 3. 初始化日志
    wd_log_init();
    WD_LOG_INFO("看门狗启动，PID=" << wd_get_my_pid());

    // 4. 初始化通知器（从配置读取）
    json cfg = WDConfigManager::instance().get_json();
    std::string ws_url = cfg.value("notifier", json::object()).value("ws_url", "");
    std::string ws_key = cfg.value("notifier", json::object()).value("ws_key", "");
    std::string http_url = cfg.value("notifier", json::object()).value("http_url", "");
    std::string http_key = cfg.value("notifier", json::object()).value("http_key", "");

    if (!ws_url.empty() || !http_url.empty()) {
        g_notifier.init(ws_url, ws_key, http_url, http_key);
        g_notifier.start();
        g_notifier.notify_watchdog_start();
        WD_LOG_INFO("通知器已启动");
    }

    // 5. 创建 OneBot
    auto ob = wd_onebot_create();
    if (ob) ob->report_start(wd_processes());

    // 6. 创建 HTTP 上报器
    auto reporter = wd_http_create();
    if (reporter) reporter->report_start(wd_processes());

    // 7. 启动所有进程
    for (auto& pc : wd_processes()) {
        if (wd_start(pc.cmd, pc.args, pc.working_dir, false, -1, pc.show_window) != WD_INVALID_PROCESS_ID) {
            pc.is_running = true;
            WD_LOG_INFO("启动进程: " << pc.name);
        }
    }

    // 8. 运行主循环
    wd_run(wd_processes(), ob.get(), reporter.get());

    // 9. 清理
    for (auto& pc : wd_processes()) {
        if (pc.is_running) wd_stop(pc.pid);
    }
    if (ob) ob->disconnect();
    if (reporter) reporter->stop();
    g_notifier.stop();
    WD_LOG_INFO("看门狗已停止");
}

// ================================================================
// 结束
// ================================================================
