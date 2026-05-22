cmake_minimum_required(VERSION 3.14)

project(scheduler_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==============================================================================
# 仅用于 scheduler 开发调试，下游项目不需要这个，删掉
set(SCHEDULER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_subdirectory("${SCHEDULER_ROOT}" scheduler)
# ==============================================================================

# 1. 引入 scheduler 子模块
# 下游项目打开这个注释，改为 scheduler 子模块路径
# add_subdirectory(third_party/scheduler)

# 2. 业务可执行文件
add_executable(scheduler_demo scheduler_demo.cpp)
target_link_libraries(scheduler_demo PRIVATE scheduler)
