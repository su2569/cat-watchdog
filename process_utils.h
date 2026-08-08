#pragma once

#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include "wdsystem.h"

#ifdef _WIN32
#include <tlhelp32.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <limits.h>
#include <sys/wait.h>
#endif

// ========== 精确进程查找 ==========

#ifdef _WIN32
#include <tlhelp32.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <limits.h>
#endif

// 获取进程的完整可执行路径
inline std::string wd_get_process_path(WDProcessId pid) {
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) return "";
    char path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameA(hProcess, 0, path, &size)) {
        CloseHandle(hProcess);
        return std::string(path);
    }
    HMODULE hMod;
    DWORD needed;
    if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &needed)) {
        if (GetModuleFileNameExA(hProcess, hMod, path, MAX_PATH)) {
            CloseHandle(hProcess);
            return std::string(path);
        }
    }
    CloseHandle(hProcess);
    return "";
#else
    char path[PATH_MAX];
    std::string link = "/proc/" + std::to_string(pid) + "/exe";
    ssize_t len = readlink(link.c_str(), path, sizeof(path) - 1);
    if (len > 0) {
        path[len] = 0;
        return std::string(path);
    }
    return "";
#endif
}

// 获取进程的完整命令行
inline std::string wd_get_process_cmdline(WDProcessId pid) {
#ifdef _WIN32
    return wd_get_process_path(pid);
#else
    std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string cmdline;
    char c;
    while (f.get(c)) {
        if (c == 0) cmdline += ' ';
        else cmdline += c;
    }
    while (!cmdline.empty() && cmdline.back() == ' ') cmdline.pop_back();
    return cmdline;
#endif
}

// 路径标准化（统一分隔符）
inline std::string wd_normalize_path(const std::string& path) {
    std::string result = path;
#ifdef _WIN32
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] == '/') result[i] = '\\';
    }
#else
    for (size_t i = 0; i < result.size(); i++) {
        if (result[i] == '\\') result[i] = '/';
    }
#endif
    return result;
}

// 查找匹配指定路径的进程 PID 列表
inline std::vector<WDProcessId> wd_find_processes_by_path(const std::string& target_path) {
    std::vector<WDProcessId> result;
    if (target_path.empty()) return result;
    std::string normalized_target = wd_normalize_path(target_path);

#ifdef _WIN32
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(hSnap, &pe)) {
        do {
            std::string path = wd_get_process_path(pe.th32ProcessID);
            if (!path.empty()) {
                std::string norm_path = wd_normalize_path(path);
                if (norm_path.find(normalized_target) != std::string::npos ||
                    normalized_target.find(norm_path) != std::string::npos) {
                    result.push_back(pe.th32ProcessID);
                }
            }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);
#else
    DIR* dir = opendir("/proc");
    if (!dir) return result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!isdigit(entry->d_name[0])) continue;
        WDProcessId pid = std::stoi(entry->d_name);
        std::string path = wd_get_process_path(pid);
        if (!path.empty()) {
            std::string norm_path = wd_normalize_path(path);
            if (norm_path.find(normalized_target) != std::string::npos ||
                normalized_target.find(norm_path) != std::string::npos) {
                result.push_back(pid);
            }
        }
    }
    closedir(dir);
#endif
    return result;
}

// 查找匹配指定命令行的进程 PID 列表（更精确）
inline std::vector<WDProcessId> wd_find_processes_by_cmdline(const std::string& target_cmd) {
    std::vector<WDProcessId> result;
    if (target_cmd.empty()) return result;
    std::string normalized_target = wd_normalize_path(target_cmd);

#ifdef _WIN32
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(hSnap, &pe)) {
        do {
            std::string cmdline = wd_get_process_cmdline(pe.th32ProcessID);
            if (!cmdline.empty()) {
                std::string norm_cmd = wd_normalize_path(cmdline);
                if (norm_cmd.find(normalized_target) != std::string::npos) {
                    result.push_back(pe.th32ProcessID);
                }
            }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);
#else
    DIR* dir = opendir("/proc");
    if (!dir) return result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!isdigit(entry->d_name[0])) continue;
        WDProcessId pid = std::stoi(entry->d_name);
        std::string cmdline = wd_get_process_cmdline(pid);
        if (!cmdline.empty()) {
            std::string norm_cmd = wd_normalize_path(cmdline);
            if (norm_cmd.find(normalized_target) != std::string::npos) {
                result.push_back(pid);
            }
        }
    }
    closedir(dir);
#endif
    return result;
}

// 获取指定进程的所有子进程 PID
inline std::vector<WDProcessId> wd_get_child_processes(WDProcessId parent_pid) {
    std::vector<WDProcessId> result;
#ifdef _WIN32
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(hSnap, &pe)) {
        do {
            if (pe.th32ParentProcessID == parent_pid) {
                result.push_back(pe.th32ProcessID);
            }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);
#else
    DIR* dir = opendir("/proc");
    if (!dir) return result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!isdigit(entry->d_name[0])) continue;
        WDProcessId pid = std::stoi(entry->d_name);
        std::string status_path = "/proc/" + std::to_string(pid) + "/status";
        std::ifstream f(status_path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("PPid:") == 0) {
                WDProcessId ppid = std::stoi(line.substr(5));
                if (ppid == parent_pid) {
                    result.push_back(pid);
                }
                break;
            }
        }
    }
    closedir(dir);
#endif
    return result;
}

// 递归获取所有后代进程 PID
inline std::vector<WDProcessId> wd_get_descendant_processes(WDProcessId parent_pid) {
    std::vector<WDProcessId> result;
    std::vector<WDProcessId> to_check;
    to_check.push_back(parent_pid);
    std::set<WDProcessId> visited;

    while (!to_check.empty()) {
        WDProcessId current = to_check.back();
        to_check.pop_back();
        if (visited.count(current)) continue;
        visited.insert(current);

        auto children = wd_get_child_processes(current);
        for (auto child : children) {
            if (!visited.count(child)) {
                result.push_back(child);
                to_check.push_back(child);
            }
        }
    }
    return result;
}

// 安全结束进程树（先结束子进程，再结束父进程）
inline bool wd_kill_process_tree(WDProcessId root_pid) {
    if (root_pid == WD_INVALID_PROCESS_ID || root_pid == 0) return false;

    auto descendants = wd_get_descendant_processes(root_pid);
    for (auto it = descendants.rbegin(); it != descendants.rend(); ++it) {
        WD_LOG_INFO("结束子进程 PID=" << *it);
        wd_kill_process_by_pid(*it);
        wd_wait_for_process_by_pid(*it, 2000);
    }

    WD_LOG_INFO("结束根进程 PID=" << root_pid);
    bool result = wd_kill_process_by_pid(root_pid);
    wd_wait_for_process_by_pid(root_pid, 5000);
    return result;
}

// 通过命令行精确查找并结束进程
inline bool wd_kill_process_by_cmdline(const std::string& cmd_pattern) {
    auto pids = wd_find_processes_by_cmdline(cmd_pattern);
    if (pids.empty()) {
        WD_LOG_WARN("未找到匹配进程: " << cmd_pattern);
        return false;
    }
    for (auto pid : pids) {
        WD_LOG_INFO("结束匹配进程 PID=" << pid << " [" << cmd_pattern << "]");
        wd_kill_process_tree(pid);
    }
    return true;
}

// 通过路径精确查找并结束进程
inline bool wd_kill_process_by_path(const std::string& path_pattern) {
    auto pids = wd_find_processes_by_path(path_pattern);
    if (pids.empty()) {
        WD_LOG_WARN("未找到匹配进程: " << path_pattern);
        return false;
    }
    for (auto pid : pids) {
        WD_LOG_INFO("结束匹配进程 PID=" << pid << " [" << path_pattern << "]");
        wd_kill_process_tree(pid);
    }
    return true;
}
