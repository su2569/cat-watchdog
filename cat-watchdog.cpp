#include "watchdog.h"
#include <iostream>

std::atomic<bool> g_running{true};
WDNotifier g_notifier;

int main(int argc, char* argv[]) {
    std::string config_path = "cw.json";
    if (argc > 1) config_path = argv[1];
    std::cout << "Cat Watchdog - Process Monitor\nConfig: " << config_path << "\n\n";
    wd_run_all(config_path);
    return 0;
}
