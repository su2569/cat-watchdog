#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <chrono>
#include <thread>
#include <atomic>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fstream>

// ============ 平台检测宏 ============
#ifdef _WIN32
    #define WD_OS_WINDOWS 1
    #define WD_PATH_SEP '\\'
    #define WD_PATH_SEP_STR "\\"
    #define WD_SYSTEM_NAME "Windows"
    
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <windows.h>
    #include <tlhelp32.h>
    #include <psapi.h>
    #include <direct.h>      // _mkdir, _chdir
    #define wd_mkdir(path) _mkdir(path)
    #define wd_chdir(path) _chdir(path)
    #define wd_getcwd(buf, size) _getcwd(buf, size)
#else
    #define WD_OS_LINUX 1
    #define WD_PATH_SEP '/'
    #define WD_PATH_SEP_STR "/"
    #define WD_SYSTEM_NAME "Linux"
    
    #include <unistd.h>
    #include <sys/wait.h>
    #include <sys/types.h>
    #include <signal.h>
    #include <spawn.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <sys/stat.h>    // mkdir
    #define wd_mkdir(path) mkdir(path, 0755)
    #define wd_chdir(path) chdir(path)
    #define wd_getcwd(buf, size) getcwd(buf, size)
#endif

// ============ 平台无关的类型别名 ============
#ifdef _WIN32
    using WDProcessHandle = HANDLE;
    using WDProcessId = DWORD;
    #define WD_INVALID_PROCESS_ID 0
    #define WD_INVALID_HANDLE nullptr
#else
    using WDProcessHandle = pid_t;
    using WDProcessId = pid_t;
    #define WD_INVALID_PROCESS_ID -1
    #define WD_INVALID_HANDLE -1
#endif

// ============ 系统信息 ============
struct WDSystemInfo {
    std::string os_name;
    std::string hostname;
    std::string current_dir;
    WDProcessId pid;
    
    WDSystemInfo() : os_name(WD_SYSTEM_NAME), pid(0) {}
};

// ============ 辅助函数 ============

// 获取当前时间戳字符串
inline std::string wd_get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ) % 1000;
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S") 
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

// 获取当前日期字符串 YYYY-MM-DD
inline std::string wd_get_current_date() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d");
    return oss.str();
}

// 跨平台睡眠（毫秒）
inline void wd_sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 跨平台睡眠（秒）
inline void wd_sleep_sec(int sec) {
    std::this_thread::sleep_for(std::chrono::seconds(sec));
}

// 创建目录（跨平台）
inline bool wd_create_directory(const std::string& path) {
#ifdef _WIN32
    return CreateDirectoryA(path.c_str(), NULL) != 0;
#else
    return mkdir(path.c_str(), 0755) == 0;
#endif
}

// 获取系统信息
inline WDProcessId wd_get_my_pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}


inline WDSystemInfo wd_get_system_info() {
    WDSystemInfo info;
    info.pid = wd_get_my_pid();
    
    char buffer[1024];
    if (wd_getcwd(buffer, sizeof(buffer)) != nullptr) {
        info.current_dir = buffer;
    }
    
#ifdef _WIN32
    char hostname[256];
    DWORD size = sizeof(hostname);
    if (GetComputerNameA(hostname, &size)) {
        info.hostname = hostname;
    }
#else
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        info.hostname = hostname;
    }
#endif
    
    return info;
}

// ============ 进程操作子程序 ============

// 1. 检查进程是否存活
inline bool wd_is_process_alive(WDProcessId pid) {
    if (pid == WD_INVALID_PROCESS_ID) return false;
    
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (handle == NULL) return false;
    DWORD exitCode;
    bool alive = (GetExitCodeProcess(handle, &exitCode) && exitCode == STILL_ACTIVE);
    CloseHandle(handle);
    return alive;
#else
    if (pid <= 0) return false;
    return (kill(pid, 0) == 0);
#endif
}

// 2. 启动外部进程
inline bool wd_start_process(
    const std::string& path,
    const std::vector<std::string>& args,
    WDProcessId& out_pid,
    WDProcessHandle& out_handle,
    const std::string& working_dir = "",
    bool show_window = true
) {
#ifdef _WIN32
    // 构建命令行
    std::string cmd = "\"" + path + "\"";
    for (const auto& arg : args) {
        cmd += " \"" + arg + "\"";
    }
    
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    std::string work_dir = working_dir.empty() ? "." : working_dir;
    
    if (!CreateProcessA(
        NULL,
        (LPSTR)cmd.c_str(),
        NULL, NULL, FALSE,
        CREATE_NO_WINDOW,
        NULL,
        work_dir.c_str(),
        &si, &pi
    )) {
        return false;
    }
    
    out_pid = pi.dwProcessId;
    out_handle = pi.hProcess;
    CloseHandle(pi.hThread);
    return true;
#else
    (void)show_window;  // Linux 下忽略窗口显示参数

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    
    // 构建参数数组
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(path.c_str()));
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    
    // 设置工作目录
    std::string work_dir = working_dir.empty() ? "." : working_dir;
    if (wd_chdir(work_dir.c_str()) != 0) {
        // 工作目录不存在，使用当前目录
    }
    
    pid_t pid;
    int ret = posix_spawn(&pid, path.c_str(), NULL, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    
    if (ret != 0) {
        return false;
    }
    
    out_pid = pid;
    out_handle = pid;
    return true;
#endif
}

// 3. 终止进程
inline bool wd_kill_process(WDProcessHandle handle) {
#ifdef _WIN32
    if (TerminateProcess(handle, 1)) {
        CloseHandle(handle);
        return true;
    }
    return false;
#else
    if (handle > 0 && kill(handle, SIGKILL) == 0) {
        return true;
    }
    return false;
#endif
}

// 4. 终止进程（通过PID）
inline bool wd_kill_process_by_pid(WDProcessId pid) {
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (handle == NULL) return false;
    bool result = TerminateProcess(handle, 1);
    CloseHandle(handle);
    return result;
#else
    if (pid > 0 && kill(pid, SIGKILL) == 0) {
        return true;
    }
    return false;
#endif
}

// 5. 等待进程结束（带超时，单位毫秒）
inline int wd_wait_for_process(WDProcessHandle handle, int timeout_ms) {
#ifdef _WIN32
    DWORD ret = WaitForSingleObject(handle, timeout_ms);
    if (ret == WAIT_OBJECT_0) {
        DWORD exitCode;
        GetExitCodeProcess(handle, &exitCode);
        CloseHandle(handle);
        return (int)exitCode;
    } else if (ret == WAIT_TIMEOUT) {
        return -2;
    }
    return -1;
#else
    int status;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        pid_t ret = waitpid(handle, &status, WNOHANG);
        if (ret == handle) {
            return WEXITSTATUS(status);
        }
        if (ret < 0) {
            return -1;
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > timeout_ms) {
            return -2;
        }
        wd_sleep_ms(50);
    }
#endif
}

// 5.0 通过PID等待进程结束（内部打开句柄）
inline int wd_wait_for_process_by_pid(WDProcessId pid, int timeout_ms) {
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (handle == NULL) return -1;
    return wd_wait_for_process(handle, timeout_ms);
#else
    return wd_wait_for_process(pid, timeout_ms);
#endif
}



// 5.1 获取进程退出原因描述
inline std::string wd_get_exit_reason(WDProcessId pid, WDProcessHandle handle) {
#ifdef _WIN32
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) {
        return "进程句柄无效";
    }
    DWORD exitCode = 0;
    if (GetExitCodeProcess(handle, &exitCode)) {
        if (exitCode == STILL_ACTIVE) {
            return "进程仍在运行（检测时可能已退出）";
        }
        switch (exitCode) {
            case 0: return "正常退出";
            case 1: return "通用错误";
            case 3: return "路径未找到";
            case 5: return "访问被拒绝";
            case 1067: return "进程被强制终止";
            case 0xC0000005: return "访问冲突/内存错误";
            case 0xC000013A: return "Ctrl+C 终止";
            case 0xC0000142: return "DLL 初始化失败";
            case 0xC0000409: return "栈缓冲区溢出";
            default: return "退出码: " + std::to_string(exitCode);
        }
    }
    return "无法获取退出原因";
#else
    (void)handle;
    if (pid <= 0) return "无效 PID";
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == 0) {
        return "进程仍在运行（检测时可能已退出）";
    } else if (result == pid) {
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) return "正常退出";
            return "退出码: " + std::to_string(code);
        }
        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            switch (sig) {
                case SIGKILL: return "被 SIGKILL 强制结束";
                case SIGTERM: return "被 SIGTERM 终止";
                case SIGSEGV: return "段错误（内存访问违规）";
                case SIGABRT: return "异常终止（abort）";
                case SIGINT:  return "被中断（Ctrl+C）";
                case SIGPIPE: return "管道破裂";
                default: return "被信号 " + std::to_string(sig) + " 终止";
            }
        }
        if (WIFSTOPPED(status)) return "被信号停止";
        return "未知状态变化";
    }
    return "进程已消失（可能已退出）";
#endif
}

// 6. 获取当前进程ID

// ========== 端口检测 ==========
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

// 检测指定端口是否在本地监听（超时 2 秒）
inline bool wd_is_port_listening(int port) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
#endif

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    // 设置非阻塞 + 超时
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    if (result < 0) {
#ifdef _WIN32
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(sock);
            WSACleanup();
            return false;
        }
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        result = select(0, NULL, &fdset, NULL, &tv);
        if (result <= 0) {
            closesocket(sock);
            WSACleanup();
            return false;
        }
        int so_error;
        int len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error != 0) {
            closesocket(sock);
            WSACleanup();
            return false;
        }
#else
        if (errno != EINPROGRESS) {
            close(sock);
            return false;
        }
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(sock, &fdset);
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        result = select(sock + 1, NULL, &fdset, NULL, &tv);
        if (result <= 0) {
            close(sock);
            return false;
        }
        int so_error;
        socklen_t len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            close(sock);
            return false;
        }
#endif
    }

#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    return true;
}

