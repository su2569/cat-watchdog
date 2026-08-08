#include "logger.hpp"
#include <filesystem>
#include <iostream>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

namespace cwd {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    if (ofs_.is_open()) {
        ofs_.close();
    }
}

void Logger::init(const std::string& dir, LogLevel level, bool console, bool file) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
    console_ = console;
    file_ = file;
    dir_ = dir;

    if (file_) {
        std::filesystem::create_directories(dir_);
        ensure_file_open();
    }
    initialized_ = true;
}

void Logger::set_level(LogLevel level) {
    level_ = level;
}

bool Logger::is_level_enabled(LogLevel level) const {
    return static_cast<int>(level) >= static_cast<int>(level_.load());
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (!is_level_enabled(level)) return;

    std::lock_guard<std::mutex> lock(mutex_);

    std::string line = "[" + get_timestamp() + "] [" + level_to_string(level) + "] " + msg;

    if (console_.load()) {
#ifdef _WIN32
        // Windows 控制台输出 UTF-8
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (level >= LogLevel::Error) {
            hOut = GetStdHandle(STD_ERROR_HANDLE);
        }
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
            WriteConsoleA(hOut, "\n", 1, &written, nullptr);
        } else {
            if (level >= LogLevel::Error) {
                std::cerr << line << std::endl;
            } else {
                std::cout << line << std::endl;
            }
        }
#else
        if (level >= LogLevel::Error) {
            std::cerr << line << std::endl;
        } else {
            std::cout << line << std::endl;
        }
#endif
    }

    if (file_.load()) {
        ensure_file_open();
        if (ofs_.is_open()) {
            ofs_ << line << std::endl;
            ofs_.flush();
        }
    }
}

void Logger::trace(const std::string& msg) { log(LogLevel::Trace, msg); }
void Logger::debug(const std::string& msg) { log(LogLevel::Debug, msg); }
void Logger::info(const std::string& msg)  { log(LogLevel::Info, msg); }
void Logger::warn(const std::string& msg)  { log(LogLevel::Warn, msg); }
void Logger::error(const std::string& msg) { log(LogLevel::Error, msg); }
void Logger::fatal(const std::string& msg) { log(LogLevel::Fatal, msg); }

void Logger::ensure_file_open() {
    if (!ofs_.is_open() || current_file_.empty()) {
        rotate_if_needed();
    }
}

void Logger::rotate_if_needed() {
    std::string date_str = get_date_str();
    std::string new_file = dir_ + "/cat-watchdog-" + date_str + ".log";

    if (current_file_ != new_file) {
        if (ofs_.is_open()) ofs_.close();
        current_file_ = new_file;
        ofs_.open(current_file_, std::ios::app);
#ifdef _WIN32
        // Windows 日志文件写入 UTF-8 BOM
        if (ofs_.is_open()) {
            unsigned char bom[] = {0xEF, 0xBB, 0xBF};
            ofs_.write(reinterpret_cast<char*>(bom), 3);
        }
#endif
    }
}

std::string Logger::level_to_string(LogLevel level) const {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "UNKNOWN";
    }
}

std::string Logger::get_timestamp() const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto time = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setw(3) << std::setfill('0') << ms.count();
    return ss.str();
}

std::string Logger::get_date_str() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d");
    return ss.str();
}

} // namespace cwd
