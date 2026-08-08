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
    std::string name;              // 进程标识名
    std::string cmd;               // 可执行文件路径
    std::vector<std::string> args; // 启动参数
    std::string working_dir;       // 工作目录
    bool restart = true;           // 崩溃后是否自动重启
    int max_restart = 0;           // 最大重启次数（0=无限）
    int restart_delay_sec = 3;     // 重启间隔（秒）
    bool show_window = true;       // 是否显示窗口（Windows）
    bool enabled = true;           // 是否启用监控

    // 运行时状态（非配置项）
    std::atomic<bool> is_running{false};
    std::atomic<int> restart_count{0};
    std::atomic<uint64_t> pid{0};
    std::atomic<uint64_t> handle{0};
    std::string last_exit_reason;
};

// OneBot 配置
struct OneBotConfig {
    bool enabled = false;
    std::string ws_url;            // WebSocket 地址
    std::string access_token;      // 访问令牌
    std::vector<std::string> notify_groups; // 通知的群号
    std::vector<std::string> notify_users;  // 通知的QQ号
    int reconnect_interval_sec = 10;
};

// HTTP 上报配置
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

// 看门狗全局配置
struct WatchdogConfig {
    int check_interval_sec = 5;    // 检查间隔
    int log_level = 2;             // 0=trace,1=debug,2=info,3=warn,4=error
    std::string log_dir = "logs";
    bool console_log = true;
    bool file_log = true;
    int restart_delay_sec = 3;     // 全局默认重启延迟
};

// 配置管理器（线程安全单例）
class ConfigManager {
public:
    static ConfigManager& instance();

    bool load(const std::string& path);
    bool reload();
    bool is_loaded() const;

    // 通用配置读取
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
