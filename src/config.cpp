#include "config.hpp"
#include "logger.hpp"
#include <fstream>
#include <sstream>

namespace cwd {

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

bool ConfigManager::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        CWD_LOG_ERROR("无法打开配置文件: " + path);
        return false;
    }

    try {
        ifs >> raw_json_;
    } catch (const std::exception& e) {
        CWD_LOG_ERROR(std::string("配置文件解析失败: ") + e.what());
        return false;
    }

    parse_watchdog(raw_json_);
    parse_processes(raw_json_);
    parse_onebot(raw_json_);
    parse_http_reporter(raw_json_);

    loaded_ = true;
    CWD_LOG_INFO("配置加载成功: " + path);
    return true;
}

bool ConfigManager::reload() {
    if (path_.empty()) return false;
    return load(path_);
}

bool ConfigManager::is_loaded() const {
    return loaded_;
}

std::string ConfigManager::get_string(const std::string& key, const std::string& default_value) const {
    return get<std::string>(key, default_value);
}

int ConfigManager::get_int(const std::string& key, int default_value) const {
    return get<int>(key, default_value);
}

bool ConfigManager::get_bool(const std::string& key, bool default_value) const {
    return get<bool>(key, default_value);
}

json ConfigManager::get_json(const std::string& key, const json& default_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return default_value;

    try {
        std::vector<std::string> parts;
        std::stringstream ss(key);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (!part.empty()) parts.push_back(part);
        }

        json current = raw_json_;
        for (const auto& p : parts) {
            if (!current.contains(p)) return default_value;
            current = current[p];
        }
        return current;
    } catch (...) {
        return default_value;
    }
}

template<typename T>
T ConfigManager::get(const std::string& key, const T& default_value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return default_value;

    try {
        std::vector<std::string> parts;
        std::stringstream ss(key);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (!part.empty()) parts.push_back(part);
        }

        json current = raw_json_;
        for (const auto& p : parts) {
            if (!current.contains(p)) return default_value;
            current = current[p];
        }
        return current.get<T>();
    } catch (...) {
        return default_value;
    }
}

// 显式实例化
template std::string ConfigManager::get(const std::string&, const std::string&) const;
template int ConfigManager::get(const std::string&, const int&) const;
template bool ConfigManager::get(const std::string&, const bool&) const;

const std::vector<ProcessConfig>& ConfigManager::processes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return processes_;
}

std::optional<ProcessConfig*> ConfigManager::find_process(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pc : processes_) {
        if (pc.name == name) return &pc;
    }
    return std::nullopt;
}

const OneBotConfig& ConfigManager::onebot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return onebot_;
}

const HttpReporterConfig& ConfigManager::http_reporter() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return http_reporter_;
}

const WatchdogConfig& ConfigManager::watchdog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return watchdog_;
}

const json& ConfigManager::raw() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return raw_json_;
}

void ConfigManager::parse_watchdog(const json& j) {
    if (!j.contains("watchdog")) return;
    const auto& w = j["watchdog"];
    watchdog_.check_interval_sec = w.value("check_interval_sec", 5);
    watchdog_.log_level = w.value("log_level", 2);
    watchdog_.log_dir = w.value("log_dir", "logs");
    watchdog_.console_log = w.value("console_log", true);
    watchdog_.file_log = w.value("file_log", true);
    watchdog_.restart_delay_sec = w.value("restart_delay_sec", 3);
}

void ConfigManager::parse_processes(const json& j) {
    processes_.clear();
    if (!j.contains("processes") || !j["processes"].is_array()) return;

    for (const auto& p : j["processes"]) {
        ProcessConfig pc;
        pc.name = p.value("name", "");
        pc.cmd = p.value("cmd", "");
        if (p.contains("args") && p["args"].is_array()) {
            for (const auto& a : p["args"]) {
                if (a.is_string()) pc.args.push_back(a.get<std::string>());
            }
        }
        pc.working_dir = p.value("working_dir", "");
        pc.restart = p.value("restart", true);
        pc.max_restart = p.value("max_restart", 0);
        pc.restart_delay_sec = p.value("restart_delay_sec", watchdog_.restart_delay_sec);
        pc.show_window = p.value("show_window", true);
        pc.enabled = p.value("enabled", true);

        if (!pc.name.empty() && !pc.cmd.empty()) {
            processes_.push_back(std::move(pc));
        }
    }
}

void ConfigManager::parse_onebot(const json& j) {
    if (!j.contains("onebot")) return;
    const auto& o = j["onebot"];
    onebot_.enabled = o.value("enabled", false);
    onebot_.ws_url = o.value("ws_url", "");
    onebot_.access_token = o.value("access_token", "");
    if (o.contains("notify_groups") && o["notify_groups"].is_array()) {
        for (const auto& g : o["notify_groups"]) {
            if (g.is_string()) onebot_.notify_groups.push_back(g.get<std::string>());
        }
    }
    if (o.contains("notify_users") && o["notify_users"].is_array()) {
        for (const auto& u : o["notify_users"]) {
            if (u.is_string()) onebot_.notify_users.push_back(u.get<std::string>());
        }
    }
    onebot_.reconnect_interval_sec = o.value("reconnect_interval_sec", 10);
}

void ConfigManager::parse_http_reporter(const json& j) {
    if (!j.contains("http_reporter")) return;
    const auto& h = j["http_reporter"];
    http_reporter_.enabled = h.value("enabled", false);
    http_reporter_.url = h.value("url", "");
    http_reporter_.method = h.value("method", "POST");
    http_reporter_.timeout_sec = h.value("timeout_sec", 10);
    http_reporter_.retry_count = h.value("retry_count", 3);
    http_reporter_.retry_delay_sec = h.value("retry_delay_sec", 5);
    http_reporter_.report_on_start = h.value("report_on_start", true);
    http_reporter_.report_on_change = h.value("report_on_change", true);
    http_reporter_.report_interval_sec = h.value("report_interval_sec", 60);

    if (h.contains("headers") && h["headers"].is_object()) {
        for (auto& [k, v] : h["headers"].items()) {
            if (v.is_string()) {
                http_reporter_.headers.emplace_back(k, v.get<std::string>());
            }
        }
    }
}

} // namespace cwd
