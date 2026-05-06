cmake_minimum_required(VERSION 3.14)

# ============================================================
# scheduler — header-only 调度库
# ------------------------------------------------------------
# 本仓库不再产出 .so / .a，也不会执行 install。
# 下游项目通过 git submodule 引入本仓后，在自己的 CMakeLists.txt 里:
#
#     add_subdirectory(third_party/scheduler)
#     target_link_libraries(my_app PRIVATE scheduler::scheduler)
#
# 即可直接 #include "Scheduler.h" / "Task.h" / ... 使用。
#
# 本 CMakeLists.txt 只在「被父级 add_subdirectory() 引入」时才有意义,
# 任何 cmake -S . -B ... 的顶层调用都会被显式拒绝, 防止误生成空 build/。
# 想跑示例请进入 examples/ 目录使用 examples/build.sh,
# 那里把上一级当作 third_party/scheduler 引入, 完整模拟下游集成。
#
# 该 CMakeLists.txt 的全部职责:
#   1. 找到 scheduler 头文件依赖的第三方库 (OpenCV / Threads)
#   2. 暴露一个 INTERFACE 目标 scheduler::scheduler,
#      把 include 路径 + 第三方依赖一起传递给下游
# ============================================================

# 拦截把本仓当顶层工程配置的用法 ---------------------------------
# CMAKE_SOURCE_DIR 是 cmake -S 指定的源根; 等于本目录就说明用户在
# 仓根直接跑 cmake, 这种调用方式只会得到一个空 build/, 没有任何
# 实际目标可编译, 直接报错引导用户走正确入口。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_LIST_DIR)
    message(FATAL_ERROR
        "scheduler 是 header-only 库，根 CMakeLists.txt 不应该作为顶层工程配置。\n"
        "  - 想跑示例:    cd examples && ./build.sh\n"
        "  - 集成到项目:  在你的 CMakeLists.txt 里写 add_subdirectory(third_party/scheduler)\n"
        "                 然后 target_link_libraries(<your_target> PRIVATE scheduler::scheduler)")
endif()

project(scheduler
    VERSION   0.1.0
    LANGUAGES CXX
)

# ============================================================
# 第三方依赖 (PUBLIC，会随 scheduler::scheduler 传递给下游)
# ------------------------------------------------------------
# OpenCV: 头文件直接 #include <opencv2/opencv.hpp>，下游必须能找到。
#         可通过 -DOpenCV_DIR=/path/to/opencv/build 指定非标准路径。
# Threads: Scheduler / Worker 中使用 std::thread + std::mutex。
# ============================================================
find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

# ============================================================
# header-only INTERFACE 目标
# ------------------------------------------------------------
# 下游统一通过别名 scheduler::scheduler 链接，避免与项目自身
# target 名冲突，也方便日后切换实现方式时不破坏调用方。
# ============================================================
add_library(scheduler INTERFACE)
add_library(scheduler::scheduler ALIAS scheduler)

target_include_directories(scheduler INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(scheduler INTERFACE
    ${OpenCV_LIBS}
    Threads::Threads
)

target_compile_features(scheduler INTERFACE cxx_std_17)
