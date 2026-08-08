#pragma once

#include "config.hpp"
#include "process.hpp"
#include "notifier.hpp"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace cwd {

// 监控结果
struct MonitorResult {
    bool success = false;
    bool was_restarted = false;
    std::string process_name;
    std::string message;
};

class WatchdogEngine {
public:
    WatchdogEngine();
    ~WatchdogEngine();

    // 初始化（加载配置、初始化日志、通知器等）
    bool initialize(const std::string& config_path);

    // 启动监控循环（阻塞）
    void run();

    // 停止监控
    void stop();

    // 单次检查所有进程（非阻塞）
    std::vector<MonitorResult> check_once();

    // 获取监控统计
    struct Stats {
        int total_checks = 0;
        int total_restarts = 0;
        int total_crashes = 0;
        std::chrono::steady_clock::time_point start_time;
    };
    Stats get_stats() const;

private:
    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
    mutable std::mutex stats_mutex_;
    Stats stats_;

    // 监控单个进程
    MonitorResult monitor_process(ProcessConfig& pc);

    // 启动进程并更新配置状态
    bool start_process(ProcessConfig& pc);

    // 清理残留
    void cleanup_residual(const ProcessConfig& pc);

    // 上报状态
    void do_periodic_report();
    std::chrono::steady_clock::time_point last_report_time_;
};

// 便捷入口函数
bool run_watchdog(const std::string& config_path = "cw.json");

} // namespace cwd
