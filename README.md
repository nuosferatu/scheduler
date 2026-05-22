# Scheduler

一个 **header-only** 的 C++17 数据驱动任务调度库。所有实现都在 `include/` 下，
下游项目通过 `git submodule` 引入本仓后直接 `#include` 头文件即可使用，
不需要编译/安装任何 `.so` 或 `.a`。

## 核心概念

- **Task**：最小执行单元。继承 `scheduler::Task`，声明它消费的数据类型
  (`consumes()`) 和生产的数据类型 (`produces()`)，并实现 `run()`。
- **TaskFlow**：一组任务 + 一组数据类型注册的集合。同一个 TaskFlow 内部
  统一管理数据类型与任务，便于按业务/模块拆分。
- **Scheduler**：注册一个或多个 TaskFlow 后，自动根据 `consumes` / `produces`
  推导任务依赖、识别外部输入、按帧（`frame_id`）调度执行。
- **DataHub / DataBuffer**：多帧数据缓存池。每种数据类型按 `data_type_id`
  注册，内部以 `frame_id` 索引，支持按容量回收旧帧。
- **Worker**：单任务工作线程。每个 Task 独占一个线程，消费帧队列。

调度行为要点：

- **数据驱动**：当某帧的某个数据到达后，所有 `consumes` 都已齐备的任务会被
  调度执行；任务执行成功后产生的 `produces` 会作为下游任务的输入触发它们。
- **外部输入自动识别**：在所有任务的 `consumes` 中，没有任何任务 `produces`
  的数据类型会被识别为外部输入，必须通过 `Scheduler::feed(...)` 喂入。
- **失败传播**：任务返回 `false` 时被标记为 `FAILED`，所有依赖其输出的下游
  任务在该帧上会被自动标记为 `SKIPPED`。
- **优先级锁**：`Task::priority()` 返回 0 表示无优先级（默认，并行执行）；
  返回 >0 则表示该任务需要持有"优先级锁"，调度器同一时刻只允许一个高优先
  任务运行（数值越小优先级越高）。

## 头文件

| 头文件          | 主要内容                                                      |
| --------------- | ------------------------------------------------------------- |
| `Scheduler.h`   | 调度器主类 `scheduler::Scheduler`                             |
| `TaskFlow.h`    | `scheduler::TaskFlow`：数据注册 + 任务集合                    |
| `Task.h`        | `scheduler::Task` 抽象基类                                    |
| `TaskContext.h` | 任务运行期 API（读写 DataHub、释放优先级锁）                  |
| `Worker.h`      | 单任务工作线程                                                |
| `DataHub.h`     | 多帧数据中心，按 `data_type_id` 管理 `DataBuffer`             |
| `DataBuffer.h`  | 单一数据类型的多帧缓存                                        |
| `DataPool.h`    | `DataBuffer` 底层内存池                                       |
| `Type.h`        | `frame_id_t` / `data_type_id_t` / `TaskStatus` 等类型定义     |

## 第三方依赖

库本身不依赖任何第三方库，只用到了 C++17 标准库 + 线程支持：

- **Threads**（`std::thread` / `std::mutex` / `std::shared_mutex`）

`scheduler` 这个 INTERFACE 目标会把 `Threads::Threads` 以 INTERFACE 方式
传给下游，下游使用 `target_link_libraries(... scheduler)` 后无需再单独
`find_package`。

> DataHub 内部按 `data_type_id` 维护 `elem_size` / `capacity` 等元信息，
> 默认按 POD 字节拷贝管理数据；调用方在 `write_data` / `read_data_lease` 时各自知道
> 真实类型，DataHub 仅做字节拷贝。非 POD 数据（如 `std::shared_ptr<T>`）
> 可在注册时提供自定义 `copy_func` / `release_slot_func`。业务侧自己的依赖
> （如图像库、点云库）由下游工程自行管理，调度库本身不引入任何第三方依赖。

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
target_link_libraries(my_app PRIVATE scheduler)
```

`add_subdirectory` 之后即可获得 `scheduler` 这个 INTERFACE 目标，include
路径与依赖会自动透传过来。

### 3. 在源码里直接 include 头文件

```cpp
#include "scheduler/Scheduler.h"
#include "scheduler/TaskFlow.h"
#include "scheduler/Task.h"
#include "scheduler/TaskContext.h"
#include "scheduler/Type.h"
```

完整可运行示例见 [`examples/scheduler_demo.cpp`](examples/scheduler_demo.cpp)。

## 数据类型注册：统一走 `DataTypeDescriptor`

每种数据类型由一个 `scheduler::DataTypeDescriptor` 描述：

- `id()` / `name()`：标识与日志名；
- `elem_size()` / `capacity()`：缓存槽大小与帧容量；
- `init_slot()` / `copy_slot()`：默认会被包装成 `init_slot_func` / `copy_func`，
  也可单独 override `copy_func()` / `release_slot_func()` / `init_slot_func()`；
- `install_into(DataHub&)`：把当前数据类型"安装"到 `DataHub` 的策略入口。
  默认实现就是 "Default 模式" —— 直接调用底层动作
  `hub.create_buffer(id, elem_size, capacity, ...)`；有特殊初始化逻辑
  （需要等待外部资源、运行期才能确定 elem_size 等）时可 override，即
  "Custom 模式"，但最终仍应在内部调用 `hub.create_buffer(...)` 完成建桶。

`TaskFlow` 内部只维护一份 descriptor 列表，`Scheduler::init_data_hub`
也只走 `descriptor->install_into(hub)` 这一条出口。Default vs Custom
的差异完全收敛到「是否 override `install_into()`」这一个点上，不再
存在 `data_registry_` / `data_names_` / `set_data_*` / `init_func_t` 这些
平行的注册路径。

> 命名约定：`DataTypeDescriptor::install_into(hub)` 是描述符层的**策略入口**
> （多态、可 override），`DataHub::create_buffer(...)` 是 hub 层的**底层动作**
> （真正分配 `DataBuffer`）。两者一上一下分工明确，不要绕过 `install_into`
> 直接调 `create_buffer`。

### 两种注册入口

两个重载都叫 `TaskFlow::add_data_type(...)`：

| 场景                                              | 推荐方式                                                                 |
| ------------------------------------------------- | ------------------------------------------------------------------------ |
| 直接给 (id, name, elem_size, capacity) 就够用     | `TaskFlow::add_data_type(id, name, elem_size, capacity, ...)`            |
| 平凡 POD / 编译期已知大小的结构体                 | `TrivialDataTypeDescriptor<T>` + `add_data_type(descriptor)`             |
| 需要自定义 `init_slot` / `copy_slot` / 懒初始化等 | 自写 `DataTypeDescriptor` 子类 + `add_data_type(descriptor)`             |

便捷重载内部就是构造一个 `DefaultDataTypeDescriptor` 后调描述符版本
的 `add_data_type(...)`；从 `Scheduler` 看，两种入口注册进来的数据类
型走的都是同一条 descriptor 初始化路径。

## 最小用法骨架

```cpp
enum class DataId : scheduler::data_type_id_t {
    INPUT  = 1,
    OUTPUT = 2,
};

class MyTask final : public scheduler::Task {
public:
    MyTask() : Task("MyTask") {}

    auto consumes() const -> const std::vector<scheduler::data_type_id_t>& override {
        static const std::vector<scheduler::data_type_id_t> v = {
            static_cast<scheduler::data_type_id_t>(DataId::INPUT),
        };
        return v;
    }
    auto produces() const -> const std::vector<scheduler::data_type_id_t>& override {
        static const std::vector<scheduler::data_type_id_t> v = {
            static_cast<scheduler::data_type_id_t>(DataId::OUTPUT),
        };
        return v;
    }

    auto run(scheduler::frame_id_t frame_id,
             scheduler::TaskContext& ctx) -> bool override {
        auto in_lease = ctx.read_data_lease(
            frame_id, static_cast<scheduler::data_type_id_t>(DataId::INPUT));
        if (!in_lease.ok()) {
            return false;
        }
        const int x = *in_lease.as<int>();
        int y = x + 1;
        ctx.write_data(
            frame_id, static_cast<scheduler::data_type_id_t>(DataId::OUTPUT), &y);
        return true;
    }
};

class MyFlow final : public scheduler::TaskFlow {
public:
    MyFlow() : TaskFlow("MyFlow") {
        // Default mode: 直接给 (id, name, elem_size, capacity)。
        add_data_type(static_cast<scheduler::data_type_id_t>(DataId::INPUT),
                      "INPUT", sizeof(int));
        add_data_type(static_cast<scheduler::data_type_id_t>(DataId::OUTPUT),
                      "OUTPUT", sizeof(int));
        add_task(std::make_unique<MyTask>());
    }
};

int main() {
    scheduler::Scheduler s;
    s.register_task_flow(std::make_unique<MyFlow>());
    if (!s.start()) return 1;

    for (scheduler::frame_id_t fid = 1; fid <= 5; ++fid) {
        int input = static_cast<int>(fid);
        s.feed(fid, static_cast<scheduler::data_type_id_t>(DataId::INPUT), &input);
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    s.stop();
}
```

`examples/scheduler_demo.cpp` 在此基础上演示了：

- 多个 TaskFlow 协同（`CalculateA` / `CalculateB`），共同消费同一份
  外部输入 `X`，并产出 `A` 与 `B`；
- 让 `frame_id == 3` 时 `X+2=Y` 任务返回 `false`，验证下游 `Y*2=Z` /
  `Z+3=A` 在该帧上会被自动标记为 `SKIPPED`；
- 使用 `TrivialDataTypeDescriptor<int>` 注册 POD 数据类型，
  无需手写 descriptor 子类。

## 构建示例

库本身是 header-only，根目录的 `CMakeLists.txt` 仅用于声明 INTERFACE 目标，
不产生可执行文件。`examples/` 是一个独立的下游工程，把上一级目录当作
`third_party/scheduler` 引入，跑通 demo 就同时验证了真实集成路径：

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
