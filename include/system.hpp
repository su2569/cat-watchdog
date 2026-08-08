#pragma once

#include <string>
#include <cstdint>

namespace cwd {

struct SystemInfo {
    double cpu_total_percent = 0.0;
    double memory_used_percent = 0.0;
    uint64_t memory_total_bytes = 0;
    uint64_t memory_used_bytes = 0;
    uint64_t uptime_seconds = 0;
    std::string hostname;
    std::string platform;
};

class SystemMonitor {
public:
    static SystemInfo get_info();
    static std::string get_timestamp();
    static uint64_t get_uptime_seconds();
    static void sleep_ms(int ms);
    static void sleep_sec(int sec);
    static void setup_signal_handlers();
};

// 全局运行标志
extern std::atomic<bool> g_running;

} // namespace cwd
