# Scheduler

一个 **header-only** 的 C++17 任务调度库。所有实现都在 `include/` 下，
下游项目通过 `git submodule` 引入本仓后直接 `#include` 头文件即可使用，
不需要编译/安装任何 `.so` 或 `.a`。

## 头文件

| 头文件             | 主要内容                              |
| ------------------ | ------------------------------------- |
| `Scheduler.h`      | 调度器主类                            |
| `Task.h`           | `Task` / `TaskFlow` 抽象基类          |
| `TaskContext.h`    | 任务运行期与 DataHub 的桥接           |
| `Worker.h`         | 单任务工作线程                        |
| `DataHub.h`        | 帧数据缓存池                          |
| `Type.h`           | `frame_id_t` / `data_type_id_t` 等别名 |

## 第三方依赖

下游项目需要在自己的环境里能找到：

- **OpenCV**（`cv::Mat` 等被作为 DataType 使用）
- **Threads**（`std::thread` / `std::mutex`）

`scheduler::scheduler` 这个 INTERFACE 目标会把这两个依赖以 PUBLIC 方式
传给下游，下游不需要再单独 `find_package`。

## 在自己的项目里使用（推荐方式）

### 1. 把本仓作为子模块加进项目

```bash
cd my_project
git submodule add <scheduler-repo-url> third_party/scheduler
git submodule update --init --recursive
```

### 2. 在 `my_project/CMakeLists.txt` 里引入

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_project LANGUAGES CXX)

add_subdirectory(third_party/scheduler)

add_executable(my_app src/main.cpp)
target_link_libraries(my_app PRIVATE scheduler::scheduler)
```

`add_subdirectory` 之后即可获得 `scheduler::scheduler` 这个目标，
include 路径与依赖会自动传递过来。

### 3. 在源码里直接 include 头文件

```cpp
#include "Scheduler.h"
#include "Task.h"
#include "TaskContext.h"
#include "Type.h"
```

完整可运行示例见 [`examples/scheduler_demo.cpp`](examples/scheduler_demo.cpp)。

## 构建示例

库本身是 header-only，根目录的 `CMakeLists.txt` 不会作为顶层工程编译
（直接 `cmake -S . -B ...` 会被显式拒绝）。`examples/` 是一个完全自洽
的下游工程，把上一级目录当作 `third_party/scheduler` 引入，跑通 demo
就同时验证了真实集成路径：

```bash
cd examples
cmake -S . -B build
cmake --build build -j
./build/scheduler_demo
```

### 库开发者一键脚本

仓根的 `build_example.sh` 是给本仓开发者准备的便捷入口：跑一遍上面
的构建流程，并把 `examples/build/compile_commands.json` 软链到仓根，
这样 clangd 在 `include/*.h` 里也能用到完整的索引。下游使用者不需要
关心这个脚本。

```bash
./build_example.sh
./examples/build/scheduler_demo
```
