# ============================================================
# scheduler 示例 — 演示如何把 scheduler 作为子模块集成到下游项目
# ------------------------------------------------------------
# 下游项目典型目录布局：
#
#   my_project/
#   ├── CMakeLists.txt
#   ├── src/main.cpp
#   └── third_party/
#       └── scheduler/        <- git submodule add <repo> third_party/scheduler
#           ├── CMakeLists.txt
#           └── include/...
#
# 在 my_project/CMakeLists.txt 中只需两步即可使用:
#
#   add_subdirectory(third_party/scheduler)
#   add_executable(my_app src/main.cpp)
#   target_link_libraries(my_app PRIVATE scheduler::scheduler)
#
# 然后 src/main.cpp 直接 #include "Scheduler.h" 等即可：
# scheduler::scheduler 会自动把 include/ 目录与 OpenCV / Threads
# 这些 PUBLIC 依赖一起带进来，无需在下游再单独 find_package。
#
# 本目录就是这套用法的最小可运行示例 —— 把上一级目录(scheduler 仓根)
# 当作 third_party/scheduler 拉进来构建。
#
# 用法:
#   cd examples
#   cmake -S . -B build
#   cmake --build build -j
#   ./build/scheduler_demo
# ============================================================

cmake_minimum_required(VERSION 3.14)

project(scheduler_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 引入 scheduler 子模块 ----
# 真实下游项目里的等价写法:
#   add_subdirectory(third_party/scheduler)
# 这里 examples/ 位于 scheduler 仓内部，所以把上一级目录当作
# 子模块根引入。二进制目录显式命名，避免与 examples 自身的目录混淆。
set(SCHEDULER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
    "scheduler 仓库根目录(下游项目里通常是 third_party/scheduler)")
add_subdirectory("${SCHEDULER_ROOT}" scheduler_build)

# ---- 业务可执行文件 ----
add_executable(scheduler_demo scheduler_demo.cpp)
target_link_libraries(scheduler_demo PRIVATE scheduler::scheduler)
