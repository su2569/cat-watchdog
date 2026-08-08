#include "notifier.hpp"
#include "logger.hpp"
#include "system.hpp"
#include <sstream>
#include <iomanip>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <arpa/inet.h>
#endif

namespace cwd {

class SimpleHttpClient {
public:
    static std::pair<bool, std::string> 请求(
        const std::string& method,
        const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body,
        int timeout_sec
    ) {
#ifdef _WIN32
        WSADATA wsa;
        static bool wsa_init = false;
        if (!wsa_init) {
            WSAStartup(MAKEWORD(2, 2), &wsa);
            wsa_init = true;
        }
#endif

        std::string host, path = "/";
        int port = 80;
        size_t pos = url.find("://");
        if (pos != std::string::npos) {
            std::string scheme = url.substr(0, pos);
            if (scheme == "https") { port = 443; }
            pos += 3;
        } else {
            pos = 0;
        }

        size_t slash = url.find('/', pos);
        if (slash != std::string::npos) {
            host = url.substr(pos, slash - pos);
            path = url.substr(slash);
        } else {
            host = url.substr(pos);
        }

        size_t colon = host.find(':');
        if (colon != std::string::npos) {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        }

#ifdef _WIN32
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) return {false, "socket creation failed"};
#else
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return {false, "socket creation failed"};
#endif

        struct hostent* he = gethostbyname(host.c_str());
        if (!he) {
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
            return {false, "DNS resolution failed"};
        }

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

#ifdef _WIN32
        DWORD timeout = timeout_sec * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
            return {false, "connection failed"};
        }

        std::string req = method + " " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: close\r\n";
        for (const auto& h : headers) {
            req += h.first + ": " + h.second + "\r\n";
        }
        req += "\r\n";
        req += body;

#ifdef _WIN32
        send(sock, req.c_str(), static_cast<int>(req.size()), 0);
#else
        send(sock, req.c_str(), req.size(), 0);
#endif

        std::string response;
        char buf[4096];
        int n;
        while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
            buf[n] = '\0';
            response += buf;
        }

#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif

        return {true, response};
    }
};

// ==================== OneBot 后端 ====================

OneBotBackend::OneBotBackend(const OneBotConfig& cfg) : cfg_(cfg) {}

OneBotBackend::~OneBotBackend() {
    shutdown();
}

bool OneBotBackend::init() {
    if (!cfg_.enabled) return false;
    running_ = true;
    worker_ = std::thread(&OneBotBackend::worker_loop, this);
    return true;
}

void OneBotBackend::shutdown() {
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    ready_ = false;
}

bool OneBotBackend::send(const Notification& msg) {
    if (!cfg_.enabled) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(msg);
    cv_.notify_one();
    return true;
}

bool OneBotBackend::is_ready() const {
    return ready_.load();
}

void OneBotBackend::worker_loop() {
    auto [ok, resp] = SimpleHttpClient::request(
        "GET", cfg_.ws_url + "/get_version_info",
        {{"Authorization", "Bearer " + cfg_.access_token}}, "", 5
    );
    if (ok && resp.find("200") != std::string::npos) {
        ready_ = true;
        CWD_LOG_INFO("OneBot backend ready: " + cfg_.ws_url);
    } else {
        CWD_LOG_WARN("OneBot connection test failed, will try sending anyway");
        ready_ = true;
    }

    while (running_) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || !running_; });

        while (!queue_.empty()) {
            Notification msg = queue_.front();
            queue_.pop();
            lock.unlock();

            std::string json_msg = build_message(msg);
            auto [success, response] = SimpleHttpClient::request(
                "POST", cfg_.ws_url + "/send_msg",
                {
                    {"Content-Type", "application/json"},
                    {"Authorization", "Bearer " + cfg_.access_token}
                },
                json_msg, 10
            );

            if (!success) {
                CWD_LOG_WARN("OneBot send failed: " + msg.title);
            }

            lock.lock();
        }
    }
}

std::string OneBotBackend::build_message(const Notification& msg) const {
    json j;
    j["message_type"] = "private";
    if (!cfg_.notify_groups.empty()) {
        j["message_type"] = "group";
        j["group_id"] = cfg_.notify_groups[0];
    } else if (!cfg_.notify_users.empty()) {
        j["user_id"] = cfg_.notify_users[0];
    }

    std::string text = "[" + msg.title + "]\n" + msg.content;
    if (!msg.process_name.empty()) {
        text += "\nProcess: " + msg.process_name;
    }

    j["message"] = json::array();
    j["message"].push_back({{"type", "text"}, {"data", {{"text", text}}}});

    return j.dump();
}

// ==================== HTTP 上报后端 ====================

HttpReporterBackend::HttpReporterBackend(const HttpReporterConfig& cfg) : cfg_(cfg) {}

HttpReporterBackend::~HttpReporterBackend() {
    shutdown();
}

bool HttpReporterBackend::init() {
    if (!cfg_.enabled) return false;
    running_ = true;
    ready_ = true;
    last_report_ = std::chrono::steady_clock::now();
    worker_ = std::thread(&HttpReporterBackend::worker_loop, this);
    CWD_LOG_INFO("HTTP reporter started: " + cfg_.url);
    return true;
}

void HttpReporterBackend::shutdown() {
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    ready_ = false;
}

bool HttpReporterBackend::send(const Notification& msg) {
    if (!cfg_.enabled) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(msg);
    cv_.notify_one();
    return true;
}

bool HttpReporterBackend::is_ready() const {
    return ready_.load();
}

bool HttpReporterBackend::report_status(const std::vector<ProcessConfig>& processes) {
    if (!cfg_.enabled || !ready_) return false;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_report_).count();
    if (elapsed < cfg_.report_interval_sec) return true;

    std::string body = build_status_json(processes);
    bool ok = do_http_request(body);
    if (ok) last_report_ = now;
    return ok;
}

void HttpReporterBackend::worker_loop() {
    while (running_) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(1), [this] { return !queue_.empty() || !running_; });

        while (!queue_.empty()) {
            Notification msg = queue_.front();
            queue_.pop();
            lock.unlock();

            json j;
            j["type"] = "notification";
            j["title"] = msg.title;
            j["content"] = msg.content;
            j["process_name"] = msg.process_name;
            j["priority"] = msg.priority;
            j["timestamp"] = msg.timestamp;

            do_http_request(j.dump());

            lock.lock();
        }
    }
}

bool HttpReporterBackend::do_http_request(const std::string& body) {
    std::vector<std::pair<std::string, std::string>> hdrs = cfg_.headers;
    hdrs.push_back({"Content-Type", "application/json"});

    int retries = cfg_.retry_count;
    while (retries >= 0) {
        auto [ok, resp] = SimpleHttpClient::request(
            cfg_.method, cfg_.url, hdrs, body, cfg_.timeout_sec
        );
        if (ok && resp.find("HTTP/1.1 2") != std::string::npos) {
            return true;
        }
        if (retries > 0) {
            SystemMonitor::sleep_sec(cfg_.retry_delay_sec);
        }
        retries--;
    }
    return false;
}

std::string HttpReporterBackend::build_status_json(const std::vector<ProcessConfig>& processes) const {
    json j;
    j["type"] = "status_report";
    j["timestamp"] = SystemMonitor::get_timestamp();
    j["hostname"] = SystemMonitor::get_info().hostname;

    json procs = json::array();
    for (const auto& p : processes) {
        json pj;
        pj["name"] = p.name;
        pj["enabled"] = p.enabled;
        pj["running"] = p.is_running.load();
        pj["pid"] = p.pid.load();
        pj["restart_count"] = p.restart_count.load();
        pj["last_exit_reason"] = p.last_exit_reason;
        procs.push_back(pj);
    }
    j["processes"] = procs;

    auto sys = SystemMonitor::get_info();
    j["system"] = {
        {"cpu_percent", sys.cpu_total_percent},
        {"memory_used_percent", sys.memory_used_percent},
        {"memory_total_bytes", sys.memory_total_bytes},
        {"memory_used_bytes", sys.memory_used_bytes},
        {"uptime_seconds", sys.uptime_seconds}
    };

    return j.dump();
}

// ==================== 通知中心 ====================

Notifier& Notifier::instance() {
    static Notifier inst;
    return inst;
}

void Notifier::init(const OneBotConfig& ob_cfg, const HttpReporterConfig& http_cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    backends_.clear();

    auto ob = std::make_unique<OneBotBackend>(ob_cfg);
    if (ob->init()) {
        backends_.push_back(std::move(ob));
    }

    auto http = std::make_unique<HttpReporterBackend>(http_cfg);
    if (http->init()) {
        backends_.push_back(std::move(http));
    }
}

void Notifier::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& b : backends_) {
        b->shutdown();
    }
    backends_.clear();
}

void Notifier::notify(const Notification& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& b : backends_) {
        if (b->is_ready()) {
            b->send(msg);
        }
    }
}

void Notifier::notify_process_crashed(const std::string& name, const std::string& reason) {
    Notification n;
    n.type = Notification::ProcessCrashed;
    n.title = "Process Crashed";
    n.content = "Process " + name + " crashed. Reason: " + reason;
    n.process_name = name;
    n.priority = 1;
    n.timestamp = SystemMonitor::get_timestamp();
    notify(n);
}

void Notifier::notify_process_restarted(const std::string& name, int restart_count) {
    Notification n;
    n.type = Notification::ProcessRestarted;
    n.title = "Process Restarted";
    n.content = "Process " + name + " restarted (count: " + std::to_string(restart_count) + ")";
    n.process_name = name;
    n.priority = 0;
    n.timestamp = SystemMonitor::get_timestamp();
    notify(n);
}

void Notifier::notify_process_started(const std::string& name) {
    Notification n;
    n.type = Notification::ProcessStarted;
    n.title = "Process Started";
    n.content = "Process " + name + " started";
    n.process_name = name;
    n.timestamp = SystemMonitor::get_timestamp();
    notify(n);
}

void Notifier::notify_process_stopped(const std::string& name) {
    Notification n;
    n.type = Notification::ProcessStopped;
    n.title = "Process Stopped";
    n.content = "Process " + name + " stopped";
    n.process_name = name;
    n.timestamp = SystemMonitor::get_timestamp();
    notify(n);
}

void Notifier::notify_system_alert(const std::string& title, const std::string& content) {
    Notification n;
    n.type = Notification::SystemAlert;
    n.title = title;
    n.content = content;
    n.priority = 1;
    n.timestamp = SystemMonitor::get_timestamp();
    notify(n);
}

void Notifier::notify_status(const std::vector<ProcessConfig>& processes) {
    for (auto& b : backends_) {
        if (b->name() == "HTTP") {
            auto* http = dynamic_cast<HttpReporterBackend*>(b.get());
            if (http) http->report_status(processes);
        }
    }
}

bool Notifier::has_ready_backend() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& b : backends_) {
        if (b->is_ready()) return true;
    }
    return false;
}

} // namespace cwd
