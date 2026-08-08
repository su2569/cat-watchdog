#include "system.hpp"
#include "logger.hpp"
#include <csignal>
#include <thread>
#include <chrono>

#ifdef _WIN32
    #include <windows.h>
    #include <intrin.h>
#else
    #include <sys/utsname.h>
    #include <unistd.h>
#endif

namespace cwd {

std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
    CWD_LOG_INFO("收到信号 " + std::to_string(sig) + ", 正在退出...");
    g_running = false;
}

SystemInfo SystemMonitor::get_info() {
    SystemInfo info;
    info.hostname = "unknown";
    info.platform = "unknown";

#ifdef _WIN32
    info.platform = "Windows";
    // CPU
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        // 简化处理
    }
    // Memory
    MEMORYSTATUSEX mem = {0};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        info.memory_total_bytes = mem.ullTotalPhys;
        info.memory_used_bytes = mem.ullTotalPhys - mem.ullAvailPhys;
        info.memory_used_percent = mem.dwMemoryLoad;
    }
    // Uptime
    info.uptime_seconds = GetTickCount64() / 1000;
    // Hostname
    char buf[256];
    DWORD len = sizeof(buf);
    if (GetComputerNameA(buf, &len)) {
        info.hostname = std::string(buf, len);
    }
#else
    struct utsname un;
    if (uname(&un) == 0) {
        info.platform = std::string(un.sysname) + " " + un.release;
    }

    // Memory
    auto meminfo = [](const std::string& key) -> uint64_t {
        std::ifstream ifs("/proc/meminfo");
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.find(key) == 0) {
                size_t pos = line.find_first_of("0123456789");
                if (pos != std::string::npos) {
                    return std::stoull(line.substr(pos)) * 1024;
                }
            }
        }
        return 0;
    };
    uint64_t total = meminfo("MemTotal");
    uint64_t available = meminfo("MemAvailable");
    if (total > 0) {
        info.memory_total_bytes = total;
        info.memory_used_bytes = total - available;
        info.memory_used_percent = static_cast<double>(total - available) / total * 100.0;
    }

    // Uptime
    std::ifstream ifs("/proc/uptime");
    double up = 0;
    ifs >> up;
    info.uptime_seconds = static_cast<uint64_t>(up);

    // Hostname
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        info.hostname = buf;
    }
#endif

    return info;
}

std::string SystemMonitor::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

uint64_t SystemMonitor::get_uptime_seconds() {
    return get_info().uptime_seconds;
}

void SystemMonitor::sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void SystemMonitor::sleep_sec(int sec) {
    std::this_thread::sleep_for(std::chrono::seconds(sec));
}

void SystemMonitor::setup_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifndef _WIN32
    std::signal(SIGQUIT, signal_handler);
    std::signal(SIGHUP, signal_handler);
#endif
}

} // namespace cwd
