// scheduler_demo.cpp
//
// 一个最小可跑的调度器 demo, 演示完整使用流程:
//
//   feed(LEFT_IMAGE) ──► [AddTen]      ──LEFT_IMAGE_PROCESSED──► [MultiplyTwo]
//                                                                      │
//                                                              DISPARITY_IMAGE
//                                                                      │
//                                                                      ▼
//                                                                  [Print]
//
// 数据全部用 int (POD), 这样 DataHub 默认的 memcpy copy_func 就够了, 不需要
// 给 cv::Mat 之类的非 POD 写自定义 copy_func。
//
// 编译参考 (header-only, 直接 g++ 即可):
//   g++ -std=c++17 -O2 -Iinclude examples/scheduler_demo.cpp -lpthread -o scheduler_demo
//
// 注: 这里复用了项目已有的 DataTypeId 枚举值 (LEFT_IMAGE / LEFT_IMAGE_PROCESSED /
//     DISPARITY_IMAGE), 仅作为通用的"数据槽位"使用, 与立体视觉的语义无关。

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "Scheduler.h"
#include "Task.h"
#include "TaskContext.h"
#include "Type.h"

using scheduler::DataTypeId;
using scheduler::frame_id_t;
using scheduler::Scheduler;
using scheduler::Task;
using scheduler::TaskContext;
using scheduler::TaskFlowTemplate;

class AddTenTask final : public Task {
public:
    AddTenTask() : Task("Add10") {
        printf("[AddTenTask] 创建任务 '%s'\n", name().c_str());
    }

    const std::vector<DataTypeId>& consumes() const override {
        static const std::vector<DataTypeId> v = {
            DataTypeId::LEFT_IMAGE
        };
        return v;
    }

    const std::vector<DataTypeId>& produces() const override {
        static const std::vector<DataTypeId> v = {
            DataTypeId::LEFT_IMAGE_PROCESSED
        };
        return v;
    }

    bool run(frame_id_t fid, TaskContext& ctx) override {
        // 获取输入数据x
        int x = 0;
        if (!ctx.get_data(fid, DataTypeId::LEFT_IMAGE, x)) {
            return false;
        }

        // 失效测试：让第3帧失败，测试后续任务是否被跳过
        if (fid == 3) {
            printf("  [Add10] Failed: frame_id = %u\n", fid);
            return false;
        }

        // 计算输出数据y
        int y = x + 10;
        printf("  [Add10] Success: frame_id = %u, input = %d, output = input + 10 = %d\n", fid, x, y);

        ctx.set_data(fid, DataTypeId::LEFT_IMAGE_PROCESSED, y);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return true;
    }
};

class MultiplyTwoTask final : public Task {
public:
    MultiplyTwoTask() : Task("Mul2") {
        printf("[MultiplyTwoTask] 创建任务 '%s'\n", name().c_str());
    }

    const std::vector<DataTypeId>& consumes() const override {
        static const std::vector<DataTypeId> v = {
            DataTypeId::LEFT_IMAGE_PROCESSED
        };
        return v;
    }

    const std::vector<DataTypeId>& produces() const override {
        static const std::vector<DataTypeId> v = {
            DataTypeId::DISPARITY_IMAGE
        };
        return v;
    }

    bool run(frame_id_t fid, TaskContext& ctx) override {
        // 获取输入数据x
        int x = 0;
        if (!ctx.get_data(fid, DataTypeId::LEFT_IMAGE_PROCESSED, x)) {
            return false;
        }

        // 计算输出数据y
        int y = x * 2;
        printf("  [Mul2] Success: frame_id = %u, input = %d, output = input * 2 = %d\n", fid, x, y);

        ctx.set_data(fid, DataTypeId::DISPARITY_IMAGE, y);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return true;
    }
};

class PrintTask final : public Task {
public:
    explicit PrintTask() : Task("PrintResult") {
        printf("[PrintTask] 创建任务 '%s'\n", name().c_str());
    }

    const std::vector<DataTypeId>& consumes() const override {
        static const std::vector<DataTypeId> v = {
            DataTypeId::DISPARITY_IMAGE
        };
        return v;
    }

    const std::vector<DataTypeId>& produces() const override {
        static const std::vector<DataTypeId> v = {};
        return v;
    }

    bool run(frame_id_t fid, TaskContext& ctx) override {
        // 获取输入数据x
        int result = 0;
        if (!ctx.get_data(fid, DataTypeId::DISPARITY_IMAGE, result)) {
            return false;
        }

        // 打印输出数据x
        printf("  [PrintResult] Success: frame_id = %u, result = %d\n", fid, result);
        return true;
    }
};

class DemoFlow final : public TaskFlowTemplate {
public:
    explicit DemoFlow() : TaskFlowTemplate("DemoFlow") {
        printf("[DemoFlow] >>> 开始创建任务流 '%s'\n", name().c_str());
        // 注册数据元信息到任务流
        register_data<int>(DataTypeId::LEFT_IMAGE);
        register_data<int>(DataTypeId::LEFT_IMAGE_PROCESSED);
        register_data<int>(DataTypeId::DISPARITY_IMAGE);

        // 添加任务到任务流
        add_task(std::make_unique<AddTenTask>());
        add_task(std::make_unique<MultiplyTwoTask>());
        add_task(std::make_unique<PrintTask>());
        printf("[DemoFlow] <<< 任务流 '%s' 创建完成\n", name().c_str());
    }
};

int main() {
    printf("==================================\n");
    printf("    DemoFlow: y = (x + 10) * 2    \n");
    printf("==================================\n");

    Scheduler scheduler;
    scheduler.register_task_flow(std::make_unique<DemoFlow>());

    if (!scheduler.start()) {
        printf("[scheduler_demo] Scheduler start failed\n");
        return 1;
    }
    printf("[scheduler_demo] Scheduler started\n");

    for (frame_id_t id = 1; id <= 5; ++id) {
        int input = static_cast<int>(id) * 2;
        printf("[scheduler_demo] Feed data: frame = %u, input = %d\n", id, input);
        if (!scheduler.feed(id, DataTypeId::LEFT_IMAGE, input)) {
            printf("[scheduler_demo] Feed failed at frame %u\n", id);
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(20));
    scheduler.stop();
    printf("[scheduler_demo] Scheduler stopped\n");
    return 0;
}
