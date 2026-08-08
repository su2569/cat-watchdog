#include "process.hpp"
#include "logger.hpp"
#include <cstring>
#include <algorithm>

#ifdef _WIN32
    #ifdef UNICODE
        #undef UNICODE
    #endif
    #ifdef _UNICODE
        #undef _UNICODE
    #endif
    #include <windows.h>
    #include <tlhelp32.h>
    #include <psapi.h>
#else
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <signal.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <dirent.h>
#endif

namespace cwd {

ProcessStartResult ProcessManager::start(
    const std::string& cmd,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    bool show_window
) {
    ProcessStartResult result;

#ifdef _WIN32
    std::string cmdline = "\"" + cmd + "\"";
    for (const auto& arg : args) {
        cmdline += " \"" + arg + "\"";
    }

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    if (!show_window) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }

    PROCESS_INFORMATION pi = {0};
    std::string work_dir = working_dir.empty() ? "" : working_dir;

    BOOL created = CreateProcessA(
        nullptr,
        &cmdline[0],
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        work_dir.empty() ? nullptr : work_dir.c_str(),
        &si,
        &pi
    );

    if (!created) {
        result.error_msg = "CreateProcess failed: " + std::to_string(GetLastError());
        CWD_LOG_ERROR(result.error_msg);
        return result;
    }

    result.success = true;
    result.pid = static_cast<uint64_t>(pi.dwProcessId);
    result.handle = reinterpret_cast<uint64_t>(pi.hProcess);
    CloseHandle(pi.hThread);

#else
    pid_t pid = fork();
    if (pid < 0) {
        result.error_msg = "fork failed: " + std::string(strerror(errno));
        CWD_LOG_ERROR(result.error_msg);
        return result;
    }

    if (pid == 0) {
        if (!working_dir.empty()) {
            if (chdir(working_dir.c_str()) != 0) {
                _exit(126);
            }
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(cmd.c_str()));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        if (!show_window) {
            int fd = open("/dev/null", O_RDWR);
            if (fd >= 0) {
                dup2(fd, STDIN_FILENO);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }

        execvp(cmd.c_str(), argv.data());
        _exit(127);
    }

    result.success = true;
    result.pid = static_cast<uint64_t>(pid);
    result.handle = 0;
#endif

    return result;
}

bool ProcessManager::stop(uint64_t pid, uint64_t handle) {
    (void)handle; // 在 Linux 上不使用
    if (pid == INVALID_PROC_ID) return false;

#ifdef _WIN32
    if (handle != 0) {
        HANDLE h = reinterpret_cast<HANDLE>(handle);
        return TerminateProcess(h, 1) != 0;
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok != 0;
#else
    return kill(static_cast<pid_t>(pid), SIGTERM) == 0;
#endif
}

bool ProcessManager::is_alive(uint64_t pid) {
    if (pid == INVALID_PROC_ID) return false;

#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return ok && code == STILL_ACTIVE;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

ProcessStatus ProcessManager::get_status(uint64_t pid, uint64_t handle) {
    (void)handle; // 在 Linux 上不使用
    ProcessStatus status;
    status.pid = pid;
    status.alive = is_alive(pid);

    if (!status.alive) {
#ifdef _WIN32
        if (handle != 0) {
            HANDLE h = reinterpret_cast<HANDLE>(handle);
            DWORD code = 0;
            if (GetExitCodeProcess(h, &code)) {
                status.exit_code = static_cast<int>(code);
            }
        }
#else
        int stat = 0;
        pid_t result = waitpid(static_cast<pid_t>(pid), &stat, WNOHANG);
        if (result == static_cast<pid_t>(pid)) {
            if (WIFEXITED(stat)) status.exit_code = WEXITSTATUS(stat);
            else if (WIFSIGNALED(stat)) status.exit_code = -WTERMSIG(stat);
        }
#endif
    }

    return status;
}

std::string ProcessManager::get_exit_reason(uint64_t pid, uint64_t handle) {
    auto status = get_status(pid, handle);
    if (status.alive) return "still running";

    if (status.exit_code == 0) return "exited normally";
    if (status.exit_code > 0) return "exited with code " + std::to_string(status.exit_code);
    if (status.exit_code < 0) return "killed by signal " + std::to_string(-status.exit_code);
    return "unknown exit status";
}

std::vector<uint64_t> ProcessManager::find_pid_by_name(const std::string& name) {
    std::vector<uint64_t> pids;
    if (name.empty()) return pids;

    // 提取文件名（不含路径）
    size_t pos = name.find_last_of("/\\");
    std::string base_name = (pos != std::string::npos) ? name.substr(pos + 1) : name;

#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    if (Process32First(snapshot, &pe)) {
        do {
            std::string proc_name(pe.szExeFile);
            if (proc_name == base_name || proc_name.find(base_name) != std::string::npos) {
                pids.push_back(static_cast<uint64_t>(pe.th32ProcessID));
            }
        } while (Process32Next(snapshot, &pe));
    }
    CloseHandle(snapshot);
#else
    DIR* dir = opendir("/proc");
    if (!dir) return pids;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        char* endptr;
        long pid_val = strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0') continue;

        std::string cmdline_path = std::string("/proc/") + entry->d_name + "/comm";
        int fd = open(cmdline_path.c_str(), O_RDONLY);
        if (fd < 0) continue;

        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;

        buf[n] = '\0';
        // 移除末尾换行
        if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';

        std::string proc_name(buf);
        if (proc_name == base_name || proc_name.find(base_name) != std::string::npos) {
            pids.push_back(static_cast<uint64_t>(pid_val));
        }
    }
    closedir(dir);
#endif

    return pids;
}

bool ProcessManager::kill_by_cmdline(const std::string& cmd) {
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    if (Process32First(snapshot, &pe)) {
        do {
            std::string proc_name(pe.szExeFile);
            if (proc_name.find(cmd) != std::string::npos || cmd.find(proc_name) != std::string::npos) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) {
                    TerminateProcess(h, 1);
                    CloseHandle(h);
                }
            }
        } while (Process32Next(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return true;
#else
    std::string pkill_cmd = "pkill -f " + cmd + " 2>/dev/null";
    int ret = system(pkill_cmd.c_str());
    return ret == 0;
#endif
}

bool ProcessManager::kill_by_path(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    std::string name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    if (name.empty()) return false;
    return kill_by_cmdline(name);
}

std::string ProcessManager::exec(const std::string& cmd) {
#ifdef _WIN32
    return "";
#else
    std::string result;
    char buffer[256];
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
#endif
}

std::string ProcessManager::env(const std::string& name) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : "";
}

} // namespace cwd
