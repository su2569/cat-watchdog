#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <sstream>
#include <iostream>
#include <iomanip>

namespace cwd {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5
};

class Logger {
public:
    static Logger& instance();

    void init(const std::string& dir, LogLevel level, bool console, bool file);
    void set_level(LogLevel level);

    void log(LogLevel level, const std::string& msg);
    void trace(const std::string& msg);
    void debug(const std::string& msg);
    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);
    void fatal(const std::string& msg);

    bool is_level_enabled(LogLevel level) const;

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    mutable std::mutex mutex_;
    std::atomic<bool> initialized_{false};
    std::atomic<LogLevel> level_{LogLevel::Info};
    std::atomic<bool> console_{true};
    std::atomic<bool> file_{true};
    std::string dir_;
    std::string current_file_;
    std::ofstream ofs_;
    std::chrono::system_clock::time_point last_rotate_;

    void ensure_file_open();
    void rotate_if_needed();
    std::string level_to_string(LogLevel level) const;
    std::string get_timestamp() const;
    std::string get_date_str() const;
};

// 便捷宏
#define CWD_LOG_TRACE(msg) cwd::Logger::instance().trace(msg)
#define CWD_LOG_DEBUG(msg) cwd::Logger::instance().debug(msg)
#define CWD_LOG_INFO(msg)  cwd::Logger::instance().info(msg)
#define CWD_LOG_WARN(msg)  cwd::Logger::instance().warn(msg)
#define CWD_LOG_ERROR(msg) cwd::Logger::instance().error(msg)
#define CWD_LOG_FATAL(msg) cwd::Logger::instance().fatal(msg)

} // namespace cwd
