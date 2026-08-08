cmake_minimum_required(VERSION 3.14)
project(cat-watchdog VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 平台检测
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
    set(PLATFORM_LIBS ws2_32)
elseif(UNIX)
    set(PLATFORM_LIBS pthread)
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Fetch nlohmann/json
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

# 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/include)

# 源文件
set(SOURCES
    src/main.cpp
    src/config.cpp
    src/logger.cpp
    src/process.cpp
    src/system.cpp
    src/notifier.cpp
    src/watchdog.cpp
)

add_executable(cat-watchdog ${SOURCES})

target_link_libraries(cat-watchdog
    PRIVATE
        nlohmann_json::nlohmann_json
        ${PLATFORM_LIBS}
)

# 安装
install(TARGETS cat-watchdog DESTINATION bin)
install(FILES config/cw.json.example DESTINATION share/cat-watchdog)
install(FILES tools/sender.py DESTINATION share/cat-watchdog/tools)
