#pragma once

#include "config.hpp"
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>

namespace cwd {

// 通知消息
struct Notification {
    enum Type {
        ProcessCrashed,
        ProcessRestarted,
        ProcessStarted,
        ProcessStopped,
        SystemAlert,
        StatusReport,
        Custom
    };

    Type type = Custom;
    std::string title;
    std::string content;
    std::string process_name;
    int priority = 0; // 0=normal, 1=high
    std::string timestamp;
};

// 通知后端接口
class INotifierBackend {
public:
    virtual ~INotifierBackend() = default;
    virtual bool init() = 0;
    virtual void shutdown() = 0;
    virtual bool send(const Notification& msg) = 0;
    virtual bool is_ready() const = 0;
    virtual std::string name() const = 0;
};

// OneBot 后端
class OneBotBackend : public INotifierBackend {
public:
    explicit OneBotBackend(const OneBotConfig& cfg);
    ~OneBotBackend() override;

    bool init() override;
    void shutdown() override;
    bool send(const Notification& msg) override;
    bool is_ready() const override;
    std::string name() const override { return "OneBot"; }

private:
    OneBotConfig cfg_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Notification> queue_;

    void worker_loop();
    bool connect_ws();
    bool send_ws_message(const std::string& json_msg);
    std::string build_message(const Notification& msg) const;
};

// HTTP 上报后端
class HttpReporterBackend : public INotifierBackend {
public:
    explicit HttpReporterBackend(const HttpReporterConfig& cfg);
    ~HttpReporterBackend() override;

    bool init() override;
    void shutdown() override;
    bool send(const Notification& msg) override;
    bool is_ready() const override;
    std::string name() const override { return "HTTP"; }

    bool report_status(const std::vector<ProcessConfig>& processes);

private:
    HttpReporterConfig cfg_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Notification> queue_;
    std::chrono::steady_clock::time_point last_report_;

    void worker_loop();
    bool do_http_request(const std::string& body);
    std::string build_status_json(const std::vector<ProcessConfig>& processes) const;
};

// 通知中心（多后端聚合）
class Notifier {
public:
    static Notifier& instance();

    void init(const OneBotConfig& ob_cfg, const HttpReporterConfig& http_cfg);
    void shutdown();

    void notify(const Notification& msg);
    void notify_process_crashed(const std::string& name, const std::string& reason);
    void notify_process_restarted(const std::string& name, int restart_count);
    void notify_process_started(const std::string& name);
    void notify_process_stopped(const std::string& name);
    void notify_system_alert(const std::string& title, const std::string& content);
    void notify_status(const std::vector<ProcessConfig>& processes);

    bool has_ready_backend() const;

private:
    Notifier() = default;
    ~Notifier() = default;
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<INotifierBackend>> backends_;
};

} // namespace cwd
