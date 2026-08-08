#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <optional>

namespace cwd {

using json = nlohmann::json;

// 进程配置
struct ProcessConfig {
    std::string name;
    std::string cmd;
    std::vector<std::string> args;
    std::string working_dir;
    bool restart = true;
    int max_restart = 0;
    int restart_delay_sec = 3;
    bool show_window = true;
    bool enabled = true;

    // 运行时状态（非配置项）
    std::atomic<bool> is_running{false};
    std::atomic<int> restart_count{0};
    std::atomic<uint64_t> pid{0};
    std::atomic<uint64_t> handle{0};
    std::string last_exit_reason;

    // 默认构造
    ProcessConfig() = default;

    // 拷贝构造（手动处理 atomic）
    ProcessConfig(const ProcessConfig& other)
        : name(other.name), cmd(other.cmd), args(other.args),
          working_dir(other.working_dir), restart(other.restart),
          max_restart(other.max_restart), restart_delay_sec(other.restart_delay_sec),
          show_window(other.show_window), enabled(other.enabled),
          is_running(other.is_running.load()),
          restart_count(other.restart_count.load()),
          pid(other.pid.load()),
          handle(other.handle.load()),
          last_exit_reason(other.last_exit_reason) {}

    // 拷贝赋值
    ProcessConfig& operator=(const ProcessConfig& other) {
        if (this != &other) {
            name = other.name;
            cmd = other.cmd;
            args = other.args;
            working_dir = other.working_dir;
            restart = other.restart;
            max_restart = other.max_restart;
            restart_delay_sec = other.restart_delay_sec;
            show_window = other.show_window;
            enabled = other.enabled;
            is_running = other.is_running.load();
            restart_count = other.restart_count.load();
            pid = other.pid.load();
            handle = other.handle.load();
            last_exit_reason = other.last_exit_reason;
        }
        return *this;
    }

    // 移动构造
    ProcessConfig(ProcessConfig&& other) noexcept
        : name(std::move(other.name)), cmd(std::move(other.cmd)),
          args(std::move(other.args)), working_dir(std::move(other.working_dir)),
          restart(other.restart), max_restart(other.max_restart),
          restart_delay_sec(other.restart_delay_sec),
          show_window(other.show_window), enabled(other.enabled),
          is_running(other.is_running.load()),
          restart_count(other.restart_count.load()),
          pid(other.pid.load()),
          handle(other.handle.load()),
          last_exit_reason(std::move(other.last_exit_reason)) {}

    // 移动赋值
    ProcessConfig& operator=(ProcessConfig&& other) noexcept {
        if (this != &other) {
            name = std::move(other.name);
            cmd = std::move(other.cmd);
            args = std::move(other.args);
            working_dir = std::move(other.working_dir);
            restart = other.restart;
            max_restart = other.max_restart;
            restart_delay_sec = other.restart_delay_sec;
            show_window = other.show_window;
            enabled = other.enabled;
            is_running = other.is_running.load();
            restart_count = other.restart_count.load();
            pid = other.pid.load();
            handle = other.handle.load();
            last_exit_reason = std::move(other.last_exit_reason);
        }
        return *this;
    }
};

struct OneBotConfig {
    bool enabled = false;
    std::string ws_url;
    std::string access_token;
    std::vector<std::string> notify_groups;
    std::vector<std::string> notify_users;
    int reconnect_interval_sec = 10;
};

struct HttpReporterConfig {
    bool enabled = false;
    std::string url;
    std::string method = "POST";
    std::vector<std::pair<std::string, std::string>> headers;
    int timeout_sec = 10;
    int retry_count = 3;
    int retry_delay_sec = 5;
    bool report_on_start = true;
    bool report_on_change = true;
    int report_interval_sec = 60;
};

struct WatchdogConfig {
    int check_interval_sec = 5;
    int log_level = 2;
    std::string log_dir = "logs";
    bool console_log = true;
    bool file_log = true;
    int restart_delay_sec = 3;
};

class ConfigManager {
public:
    static ConfigManager& instance();
    bool load(const std::string& path);
    bool reload();
    bool is_loaded() const;

    template<typename T>
    T get(const std::string& key, const T& default_value = T{}) const;

    std::string get_string(const std::string& key, const std::string& default_value = "") const;
    int get_int(const std::string& key, int default_value = 0) const;
    bool get_bool(const std::string& key, bool default_value = false) const;
    json get_json(const std::string& key, const json& default_value = json::object()) const;

    const std::vector<ProcessConfig>& processes() const;
    std::optional<ProcessConfig*> find_process(const std::string& name);

    const OneBotConfig& onebot() const;
    const HttpReporterConfig& http_reporter() const;
    const WatchdogConfig& watchdog() const;
    const json& raw() const;

private:
    ConfigManager() = default;
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    mutable std::mutex mutex_;
    std::string path_;
    json raw_json_;
    bool loaded_ = false;

    std::vector<ProcessConfig> processes_;
    OneBotConfig onebot_;
    HttpReporterConfig http_reporter_;
    WatchdogConfig watchdog_;

    void parse_processes(const json& j);
    void parse_onebot(const json& j);
    void parse_http_reporter(const json& j);
    void parse_watchdog(const json& j);
};

} // namespace cwd
