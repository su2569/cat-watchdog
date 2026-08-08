#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>
#include "wdsystem.h"

// ============ 日志级别 ============
enum WDLogLevel {
    WD_LOG_DEBUG = 0,
    WD_LOG_INFO = 1,
    WD_LOG_WARN = 2,
    WD_LOG_ERROR = 3,
    WD_LOG_NONE = 4
};

// ============ 日志管理器（单例） ============
class WDLogger {
private:
    std::ofstream log_file;
    std::mutex mtx;
    std::string log_dir;
    std::string current_date;
    int log_level;
    bool console_output;
    bool file_output;
    bool initialized;
    
    WDLogger() : log_level(WD_LOG_INFO), console_output(true), 
                 file_output(true), initialized(false) {}
    ~WDLogger() { if (log_file.is_open()) log_file.close(); }
    
public:
    static WDLogger& instance() {
        static WDLogger logger;
        return logger;
    }
    
    // 初始化日志
    void init(const std::string& dir = "logs", int level = WD_LOG_INFO, 
              bool console = true, bool file = true) {
        std::lock_guard<std::mutex> lock(mtx);
        log_dir = dir;
        log_level = level;
        console_output = console;
        file_output = file;
        initialized = true;
        
        if (file_output) {
            // 创建目录
            wd_create_directory(dir);
            current_date = wd_get_current_date();
            open_log_file();
        }
    }
    
    // 检查是否已初始化
    bool is_initialized() const { return initialized; }
    
    // 打开当天的日志文件
    void open_log_file() {
        if (log_file.is_open()) {
            log_file.close();
        }
        
        std::string filename = log_dir + "/watchdog_" + current_date + ".log";
        log_file.open(filename, std::ios::app);
        if (!log_file.is_open()) {
            std::cerr << "[ERROR] 无法打开日志文件: " << filename << std::endl;
            file_output = false;
        }
    }
    
    // 检查日期是否变化
    void check_rotate() {
        if (!file_output) return;
        std::string today = wd_get_current_date();
        if (today != current_date) {
            current_date = today;
            open_log_file();
        }
    }
    
    // 写日志
    void write(WDLogLevel level, const std::string& message) {
        if (!initialized) {
            // 未初始化时直接输出到控制台
            if (level >= WD_LOG_WARN) {
                std::cerr << "[UNINIT] " << message << std::endl;
            } else {
                std::cout << "[UNINIT] " << message << std::endl;
            }
            return;
        }
        
        if (level < log_level) return;
        
        std::lock_guard<std::mutex> lock(mtx);
        if (file_output) {
            check_rotate();
        }
        
        std::string time_str = wd_get_timestamp();
        std::string level_str;
        switch (level) {
            case WD_LOG_DEBUG: level_str = "DEBUG"; break;
            case WD_LOG_INFO:  level_str = "INFO";  break;
            case WD_LOG_WARN:  level_str = "WARN";  break;
            case WD_LOG_ERROR: level_str = "ERROR"; break;
            default: level_str = "UNKN"; break;
        }
        
        std::string log_line = "[" + time_str + "] [" + level_str + "] " + message;
        
        // 写文件
        if (file_output && log_file.is_open()) {
            log_file << log_line << std::endl;
            log_file.flush();
        }
        
        // 输出到控制台
        if (console_output) {
            if (level >= WD_LOG_ERROR) {
                std::cerr << log_line << std::endl;
            } else {
                std::cout << log_line << std::endl;
            }
        }
    }
    
    void set_level(int level) { 
        std::lock_guard<std::mutex> lock(mtx);
        log_level = level; 
    }
    void set_console(bool enable) { 
        std::lock_guard<std::mutex> lock(mtx);
        console_output = enable; 
    }
    void set_file(bool enable) { 
        std::lock_guard<std::mutex> lock(mtx);
        file_output = enable; 
    }
    int get_level() const { return log_level; }
};

// ============ 宏定义 ============
// 支持流式输出: WD_LOG_INFO("value=" << x)
#define WD_LOG_DEBUG(msg) do { std::ostringstream _wd_oss; _wd_oss << msg; WDLogger::instance().write(WD_LOG_DEBUG, _wd_oss.str()); } while(0)
#define WD_LOG_INFO(msg)  do { std::ostringstream _wd_oss; _wd_oss << msg; WDLogger::instance().write(WD_LOG_INFO,  _wd_oss.str()); } while(0)
#define WD_LOG_WARN(msg)  do { std::ostringstream _wd_oss; _wd_oss << msg; WDLogger::instance().write(WD_LOG_WARN,  _wd_oss.str()); } while(0)
#define WD_LOG_ERROR(msg) do { std::ostringstream _wd_oss; _wd_oss << msg; WDLogger::instance().write(WD_LOG_ERROR, _wd_oss.str()); } while(0)
