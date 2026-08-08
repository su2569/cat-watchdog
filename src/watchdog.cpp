#include "watchdog.hpp"
#include "logger.hpp"
#include "system.hpp"
#include "config.hpp"
#include "process.hpp"
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cwd {

WatchdogEngine::WatchdogEngine() = default;

WatchdogEngine::~WatchdogEngine() {
    stop();
}

bool WatchdogEngine::initialize(const std::string& config_path) {
    // 1. 加载配置
    if (!ConfigManager::instance().load(config_path)) {
        std::cerr << "配置加载失败: " << config_path << std::endl;
        return false;
    }

    const auto& cfg = ConfigManager::instance();

    // 2. 初始化日志
    LogLevel level = static_cast<LogLevel>(cfg.watchdog().log_level);
    Logger::instance().init(
        cfg.watchdog().log_dir,
        level,
        cfg.watchdog().console_log,
        cfg.watchdog().file_log
    );

    CWD_LOG_INFO("============================================");
    CWD_LOG_INFO("Cat Watchdog v2.0 启动");
    CWD_LOG_INFO("配置文件: " + config_path);
    CWD_LOG_INFO("监控进程数: " + std::to_string(cfg.processes().size()));
    CWD_LOG_INFO("============================================");

    // 3. 设置信号处理
    SystemMonitor::setup_signal_handlers();

    // 4. 初始化通知器
    Notifier::instance().init(cfg.onebot(), cfg.http_reporter());

    if (Notifier::instance().has_ready_backend()) {
        CWD_LOG_INFO("通知后端已初始化");
    } else {
        CWD_LOG_INFO("未启用通知后端");
    }

    // 5. 启动前检测：查找是否已有同名进程在运行
    auto& processes = const_cast<std::vector<ProcessConfig>&>(cfg.processes());
    for (auto& pc : processes) {
        if (!pc.enabled) continue;

        auto existing_pids = ProcessManager::find_pid_by_name(pc.cmd);
        if (!existing_pids.empty()) {
            // 过滤掉看门狗自己（避免误检测）
            uint64_t self_pid = 0;
#ifdef _WIN32
            self_pid = static_cast<uint64_t>(GetCurrentProcessId());
#else
            self_pid = static_cast<uint64_t>(getpid());
#endif
            bool found_alive = false;
            for (uint64_t pid : existing_pids) {
                if (pid == self_pid) continue;
                if (ProcessManager::is_alive(pid)) {
                    pc.pid = pid;
                    pc.is_running = true;
                    found_alive = true;
                    CWD_LOG_INFO("检测到进程已在运行: " + pc.name + " PID=" + std::to_string(pid));
                    break;
                }
            }
            if (!found_alive) {
                CWD_LOG_INFO("未检测到运行中的进程: " + pc.name + "，将启动新实例");
            }
        }
    }

    stats_.start_time = std::chrono::steady_clock::now();
    return true;
}

void WatchdogEngine::run() {
    running_ = true;
    const auto& cfg = ConfigManager::instance().watchdog();

    CWD_LOG_INFO("监控循环启动，检查间隔: " + std::to_string(cfg.check_interval_sec) + " 秒");

    // 首次上报（如果启用）
    if (ConfigManager::instance().http_reporter().report_on_start) {
        Notifier::instance().notify_status(ConfigManager::instance().processes());
    }

    while (running_ && g_running) {
        auto results = check_once();

        for (const auto& r : results) {
            if (!r.success) {
                CWD_LOG_ERROR("[" + r.process_name + "] " + r.message);
            } else if (r.was_restarted) {
                CWD_LOG_WARN("[" + r.process_name + "] " + r.message);
            }
        }

        // 定期状态上报
        do_periodic_report();

        // 等待下一次检查
        for (int i = 0; i < cfg.check_interval_sec && running_ && g_running; ++i) {
            SystemMonitor::sleep_sec(1);
        }
    }

    CWD_LOG_INFO("监控循环已停止");
}

void WatchdogEngine::stop() {
    running_ = false;
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
    Notifier::instance().shutdown();
}

std::vector<MonitorResult> WatchdogEngine::check_once() {
    std::vector<MonitorResult> results;
    auto& processes = const_cast<std::vector<ProcessConfig>&>(ConfigManager::instance().processes());

    for (auto& pc : processes) {
        if (!pc.enabled) continue;
        results.push_back(monitor_process(pc));
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.total_checks++;
    }

    return results;
}

MonitorResult WatchdogEngine::monitor_process(ProcessConfig& pc) {
    MonitorResult result;
    result.process_name = pc.name;

    // 如果进程标记为未运行，尝试启动
    if (!pc.is_running.load()) {
        if (start_process(pc)) {
            result.success = true;
            result.message = "进程启动成功，PID=" + std::to_string(pc.pid.load());
            Notifier::instance().notify_process_started(pc.name);
        } else {
            result.success = false;
            result.message = "进程启动失败: " + pc.cmd;
        }
        return result;
    }

    // 检查进程是否存活
    uint64_t current_pid = pc.pid.load();
    if (!ProcessManager::is_alive(current_pid)) {
        // 进程已崩溃/退出
        std::string reason = ProcessManager::get_exit_reason(current_pid, pc.handle.load());
        pc.is_running = false;
        pc.last_exit_reason = reason;

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.total_crashes++;
        }

        CWD_LOG_WARN("进程崩溃: " + pc.name + " (PID=" + std::to_string(current_pid) + "), 原因: " + reason);
        Notifier::instance().notify_process_crashed(pc.name, reason);

        // 判断是否需要重启
        bool should_restart = pc.restart;
        if (pc.max_restart > 0 && pc.restart_count.load() >= pc.max_restart) {
            should_restart = false;
            CWD_LOG_ERROR("进程 " + pc.name + " 达到最大重启次数限制 (" + std::to_string(pc.max_restart) + ")，不再重启");
        }

        if (should_restart) {
            int delay = pc.restart_delay_sec;
            CWD_LOG_INFO("等待 " + std::to_string(delay) + " 秒后重启 " + pc.name);
            SystemMonitor::sleep_sec(delay);

            // 清理残留进程
            cleanup_residual(pc);
            SystemMonitor::sleep_sec(2);

            if (start_process(pc)) {
                result.success = true;
                result.was_restarted = true;
                result.message = "进程已重启，PID=" + std::to_string(pc.pid.load()) +
                                 " (第 " + std::to_string(pc.restart_count.load()) + " 次)";
                Notifier::instance().notify_process_restarted(pc.name, pc.restart_count.load());

                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    stats_.total_restarts++;
                }
            } else {
                result.success = false;
                result.message = "进程重启失败: " + pc.cmd;
            }
        } else {
            result.success = false;
            result.message = "进程已崩溃且不重启: " + reason;
        }

        return result;
    }

    // 进程正常运行
    result.success = true;
    return result;
}

bool WatchdogEngine::start_process(ProcessConfig& pc) {
    // 启动前再次检测，避免重复启动
    auto existing_pids = ProcessManager::find_pid_by_name(pc.cmd);
    uint64_t self_pid = 0;
#ifdef _WIN32
    self_pid = static_cast<uint64_t>(GetCurrentProcessId());
#else
    self_pid = static_cast<uint64_t>(getpid());
#endif
    for (uint64_t pid : existing_pids) {
        if (pid == self_pid) continue;
        if (ProcessManager::is_alive(pid)) {
            pc.pid = pid;
            pc.is_running = true;
            CWD_LOG_INFO("进程已在运行，直接接管: " + pc.name + " PID=" + std::to_string(pid));
            return true;
        }
    }

    auto res = ProcessManager::start(pc.cmd, pc.args, pc.working_dir, pc.show_window);

    if (res.success) {
        pc.pid = res.pid;
        pc.handle = res.handle;
        pc.is_running = true;
        pc.restart_count++;
        CWD_LOG_INFO("启动成功: " + pc.name + " PID=" + std::to_string(res.pid));
        return true;
    } else {
        CWD_LOG_ERROR("启动失败: " + pc.name + " - " + res.error_msg);
        pc.is_running = false;
        return false;
    }
}

void WatchdogEngine::cleanup_residual(const ProcessConfig& pc) {
    if (!pc.cmd.empty()) {
        CWD_LOG_INFO("清理残留进程: " + pc.cmd);
        ProcessManager::kill_by_cmdline(pc.cmd);
        ProcessManager::kill_by_path(pc.cmd);
    }
}

void WatchdogEngine::do_periodic_report() {
    const auto& http_cfg = ConfigManager::instance().http_reporter();
    if (!http_cfg.enabled || !http_cfg.report_on_change) return;

    Notifier::instance().notify_status(ConfigManager::instance().processes());
}

WatchdogEngine::Stats WatchdogEngine::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

bool run_watchdog(const std::string& config_path) {
    WatchdogEngine engine;
    if (!engine.initialize(config_path)) {
        return false;
    }
    engine.run();
    return true;
}

} // namespace cwd
