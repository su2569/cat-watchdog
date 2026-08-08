#include "watchdog.hpp"
#include "logger.hpp"
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // 设置 Windows 控制台为 UTF-8 编码，解决中文乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::string config_path = "cw.json";
    if (argc > 1) config_path = argv[1];

    std::cout << "Cat Watchdog v2.0 - Process Monitor\n";
    std::cout << "Config: " << config_path << "\n\n";

    if (!cwd::run_watchdog(config_path)) {
        std::cerr << "Failed to start watchdog. Check logs for details.\n";
        return 1;
    }
    return 0;
}
