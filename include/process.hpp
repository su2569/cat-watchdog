#pragma once

#include "config.hpp"
#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
    #include <windows.h>
    using ProcessHandle = HANDLE;
    using ProcessId = DWORD;
    constexpr uint64_t INVALID_PROC_ID = 0;
#else
    using ProcessHandle = int;
    using ProcessId = pid_t;
    constexpr uint64_t INVALID_PROC_ID = 0;
#endif

namespace cwd {

struct ProcessStartResult {
    bool success = false;
    uint64_t pid = INVALID_PROC_ID;
    uint64_t handle = 0;
    std::string error_msg;
};

struct ProcessStatus {
    bool alive = false;
    uint64_t pid = INVALID_PROC_ID;
    int exit_code = 0;
    std::string exit_reason;
    double cpu_percent = 0.0;
    uint64_t memory_bytes = 0;
};

class ProcessManager {
public:
    // 启动进程
    static ProcessStartResult start(
        const std::string& cmd,
        const std::vector<std::string>& args,
        const std::string& working_dir = "",
        bool show_window = true
    );

    // 停止进程
    static bool stop(uint64_t pid, uint64_t handle = 0);
    static bool stop_by_name(const std::string& name);

    // 检查进程是否存活
    static bool is_alive(uint64_t pid);
    static ProcessStatus get_status(uint64_t pid, uint64_t handle = 0);

    // 获取退出原因
    static std::string get_exit_reason(uint64_t pid, uint64_t handle);

    // 清理残留进程（通过命令行匹配）
    static bool kill_by_cmdline(const std::string& cmd);
    static bool kill_by_path(const std::string& path);

    // 通过进程名查找已运行的 PID（用于启动时检测）
    static std::vector<uint64_t> find_pid_by_name(const std::string& name);

    // 执行命令并获取输出（仅 Unix）
    static std::string exec(const std::string& cmd);

    // 获取环境变量
    static std::string env(const std::string& name);
};

} // namespace cwd
