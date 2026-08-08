#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <cstdio>
#include <chrono>
#include <sstream>
#include "json.hpp"
#include "logger.h"
#include "wdsystem.h"

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

// ============ 发送器进程配置 ============
struct WDSenderConfig {
    std::string python_path;       // Python解释器路径
    std::string sender_script;     // sender.py路径
    int check_interval_sec;        // 状态检查间隔（秒）
    int restart_delay_sec;         // 重启延迟
    bool auto_restart;             // 是否自动重启

    WDSenderConfig() : python_path("python"), 
                       sender_script("sender.py"),
                       check_interval_sec(60),
                       restart_delay_sec(3),
                       auto_restart(true) {}
};

// ============ 发送器管道通信 ============
class WDSenderPipe {
private:
    WDSenderConfig config;
    
    std::atomic<bool> running{false};
    std::thread check_thread;
    std::chrono::steady_clock::time_point last_alive_time;
    int consecutive_failures = 0;
    
#ifdef _WIN32
    HANDLE hChildStd_IN_Rd = nullptr;
    HANDLE hChildStd_IN_Wr = nullptr;
    HANDLE hChildStd_OUT_Rd = nullptr;
    HANDLE hChildStd_OUT_Wr = nullptr;
    HANDLE hProcess = nullptr;
    DWORD childPID = 0;
#else
    FILE* to_child = nullptr;
    FILE* from_child = nullptr;
    pid_t childPID = -1;
#endif

    std::mutex send_mutex;

    // ===== 启动子进程 =====
    bool start_process() {
#ifdef _WIN32
        SECURITY_ATTRIBUTES saAttr;
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = NULL;

        // 创建匿名管道
        if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) {
            WD_LOG_ERROR("创建stdout管道失败");
            return false;
        }
        if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
            WD_LOG_ERROR("设置管道属性失败");
            return false;
        }

        if (!CreatePipe(&hChildStd_IN_Rd, &hChildStd_IN_Wr, &saAttr, 0)) {
            WD_LOG_ERROR("创建stdin管道失败");
            return false;
        }
        if (!SetHandleInformation(hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0)) {
            WD_LOG_ERROR("设置管道属性失败");
            return false;
        }

        // 启动Python进程
        STARTUPINFOA si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.hStdError = hChildStd_OUT_Wr;
        si.hStdOutput = hChildStd_OUT_Wr;
        si.hStdInput = hChildStd_IN_Rd;
        si.dwFlags |= STARTF_USESTDHANDLES;

        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        std::string cmd = "\"" + config.python_path + "\" \"" + config.sender_script + "\"";
        
        BOOL success = CreateProcessA(
            NULL,
            const_cast<char*>(cmd.c_str()),
            NULL, NULL,
            TRUE,
            CREATE_NO_WINDOW,
            NULL,
            NULL,
            &si,
            &pi
        );

        CloseHandle(hChildStd_OUT_Wr);
        CloseHandle(hChildStd_IN_Rd);

        if (!success) {
            WD_LOG_ERROR("启动发送器进程失败: " + cmd);
            return false;
        }

        hProcess = pi.hProcess;
        CloseHandle(pi.hThread);
        childPID = pi.dwProcessId;

#else
        int pipe_in[2], pipe_out[2];
        if (pipe(pipe_in) != 0 || pipe(pipe_out) != 0) {
            WD_LOG_ERROR("创建管道失败");
            return false;
        }

        childPID = fork();
        if (childPID < 0) {
            WD_LOG_ERROR("fork失败");
            return false;
        }

        if (childPID == 0) {
            // 子进程
            close(pipe_in[1]);
            close(pipe_out[0]);
            dup2(pipe_in[0], STDIN_FILENO);
            dup2(pipe_out[1], STDOUT_FILENO);
            
            execlp(config.python_path.c_str(), 
                   config.python_path.c_str(),
                   config.sender_script.c_str(),
                   NULL);
            exit(1);
        }

        // 父进程
        close(pipe_in[0]);
        close(pipe_out[1]);
        to_child = fdopen(pipe_in[1], "w");
        from_child = fdopen(pipe_out[0], "r");
        setbuf(to_child, NULL);  // 无缓冲
#endif

        last_alive_time = std::chrono::steady_clock::now();
        consecutive_failures = 0;
        WD_LOG_INFO("发送器进程已启动, PID=" << childPID);
        return true;
    }

    // ===== 停止子进程 =====
    void stop_process() {
#ifdef _WIN32
        if (hChildStd_IN_Wr) {
            CloseHandle(hChildStd_IN_Wr);
            hChildStd_IN_Wr = nullptr;
        }
        if (hChildStd_OUT_Rd) {
            CloseHandle(hChildStd_OUT_Rd);
            hChildStd_OUT_Rd = nullptr;
        }
        if (hProcess) {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 3000);
            CloseHandle(hProcess);
            hProcess = nullptr;
        }
#else
        if (to_child) { fclose(to_child); to_child = nullptr; }
        if (from_child) { fclose(from_child); from_child = nullptr; }
        if (childPID > 0) {
            kill(childPID, SIGTERM);
            waitpid(childPID, NULL, WNOHANG);
        }
#endif
        childPID = 0;
    }

    // ===== 检查进程是否存活 =====
    bool is_process_alive() {
#ifdef _WIN32
        if (!hProcess) return false;
        DWORD exitCode;
        if (!GetExitCodeProcess(hProcess, &exitCode)) return false;
        return exitCode == STILL_ACTIVE;
#else
        // 检查管道是否可写
        return childPID > 0 && to_child && from_child;
#endif
    }

    // ===== 检查线程 =====
    void check_loop() {
        while (running) {
            wd_sleep_sec(config.check_interval_sec);
            
            if (!is_process_alive()) {
                consecutive_failures++;
                WD_LOG_WARN("发送器进程异常, 连续失败: " << consecutive_failures);
                
                if (config.auto_restart && consecutive_failures < 5) {
                    WD_LOG_INFO("重启发送器...");
                    wd_sleep_sec(config.restart_delay_sec);
                    stop_process();
                    start_process();
                } else if (consecutive_failures >= 5) {
                    WD_LOG_ERROR("发送器连续失败5次, 停止尝试");
                }
            } else {
                // 发送ping探测
                json ping = {{"type", "ping"}};
                send_raw(ping);
                // 等待pong（简化处理，只要管道没断就算活着）
                consecutive_failures = 0;
                last_alive_time = std::chrono::steady_clock::now();
            }
        }
    }

public:
    WDSenderPipe() {}
    ~WDSenderPipe() { stop(); }

    // ===== 初始化 =====
    bool init(const WDSenderConfig& cfg) {
        config = cfg;
        return true;
    }

    // ===== 从JSON配置初始化 =====
    bool init_from_json(const json& cfg_json) {
        config.python_path = cfg_json.value("python_path", "python");
        config.sender_script = cfg_json.value("sender_script", "sender.py");
        config.check_interval_sec = cfg_json.value("check_interval_sec", 60);
        config.restart_delay_sec = cfg_json.value("restart_delay_sec", 3);
        config.auto_restart = cfg_json.value("auto_restart", true);
        return true;
    }

    // ===== 启动 =====
    bool start() {
        if (running) return true;
        
        if (!start_process()) {
            WD_LOG_ERROR("发送器启动失败");
            return false;
        }
        
        running = true;
        check_thread = std::thread(&WDSenderPipe::check_loop, this);
        WD_LOG_INFO("发送器已启动并进入监控");
        return true;
    }

    // ===== 停止 =====
    void stop() {
        running = false;
        if (check_thread.joinable()) {
            check_thread.join();
        }
        stop_process();
        WD_LOG_INFO("发送器已停止");
    }

    // ===== 发送JSON到发送器 =====
    bool send_raw(const json& msg) {
        std::lock_guard<std::mutex> lock(send_mutex);
        
#ifdef _WIN32
        if (!hChildStd_IN_Wr) return false;
        
        std::string payload = msg.dump() + "\n";
        DWORD written = 0;
        BOOL success = WriteFile(hChildStd_IN_Wr, payload.c_str(), 
                                  static_cast<DWORD>(payload.size()), &written, NULL);
        
        if (!success || static_cast<int>(written) != static_cast<int>(payload.size())) {
            WD_LOG_WARN("发送器写入失败");
            return false;
        }
#else
        if (!to_child) return false;
        
        std::string payload = msg.dump() + "\n";
        if (fputs(payload.c_str(), to_child) == EOF) {
            WD_LOG_WARN("发送器写入失败");
            return false;
        }
        fflush(to_child);
#endif
        
        last_alive_time = std::chrono::steady_clock::now();
        return true;
    }

    // ===== 发送通知 =====
    bool send_notification(const std::string& type, const json& data) {
        json msg;
        msg["type"] = type;
        msg["timestamp"] = wd_get_timestamp();
        msg["data"] = data;
        return send_raw(msg);
    }

    // ===== 发送崩溃通知 =====
    bool send_crash(const std::string& name, WDProcessId pid, int restart_count, 
                     const std::string& crash_reason) {
        json data;
        data["name"] = name;
        data["pid"] = (pid == WD_INVALID_PROCESS_ID) ? 0 : pid;
        data["restart_count"] = restart_count;
        data["crash_reason"] = crash_reason;
        return send_notification("crash", data);
    }

    // ===== 发送半启动通知 =====
    bool send_half_start(const std::string& name, const std::vector<int>& ports,
                          const std::vector<bool>& port_status) {
        json data;
        data["name"] = name;
        data["ports"] = ports;
        data["port_status"] = port_status;
        return send_notification("half_start", data);
    }

    // ===== 发送看门狗启动通知 =====
    bool send_watchdog_start() {
        json data;
        data["message"] = "Cat Watchdog started";
        data["version"] = "1.0";
        return send_notification("watchdog_start", data);
    }

    // ===== 发送进程状态变更 =====
    bool send_process_change(const std::string& name, bool now_running, 
                               int restart_count, WDProcessId pid) {
        json data;
        data["name"] = name;
        data["running"] = now_running;
        data["restart_count"] = restart_count;
        data["pid"] = (pid == WD_INVALID_PROCESS_ID) ? 0 : pid;
        return send_notification("process_change", data);
    }

    // ===== 发送完整状态报告 =====
    bool send_status_report(const std::vector<WDProcessConfig>& processes,
                             const std::string& hostname) {
        int running_count = 0;
        json process_list;
        
        for (const auto& pc : processes) {
            if (pc.is_running) running_count++;
            std::string icon = pc.is_running ? "✅" : "❌";
            std::string pid_str = (pc.pid == WD_INVALID_PROCESS_ID) ? "未启动" : std::to_string(pc.pid);
            process_list += "• " + pc.name + ": " + icon + " (PID: " + pid_str + ")\n";
        }
        
        json data;
        data["running_count"] = running_count;
        data["total_count"] = processes.size();
        data["process_list"] = process_list;
        data["hostname"] = hostname;
        
        return send_notification("status_report", data);
    }

    // ===== 发送自定义文本 =====
    bool send_custom_text(const std::string& text) {
        json data;
        data["text"] = text;
        return send_notification("custom", data);
    }

    // ===== 获取PID =====
    int get_pid() const { return static_cast<int>(childPID); }
    
    // ===== 是否运行中 =====
    bool is_running() const { return running && is_process_alive(); }
    
    // ===== 获取上次存活时间 =====
    std::chrono::steady_clock::time_point get_last_alive() const {
        return last_alive_time;
    }
};

// ============ 全局发送器实例 ============
inline WDSenderPipe& get_sender() {
    static WDSenderPipe sender;
    return sender;
}
