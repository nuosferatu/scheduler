#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "Scheduler.h"
#include "Task.h"
#include "TaskContext.h"
#include "Type.h"

using scheduler::data_type_id_t;
using scheduler::frame_id_t;
using scheduler::Scheduler;
using scheduler::Task;
using scheduler::TaskContext;
using scheduler::TaskFlow;

namespace demo {

enum class DataTypeId : data_type_id_t {
    LEFT_IMAGE            = 0,
    RIGHT_IMAGE           = 1,
    LEFT_IMAGE_PROCESSED  = 2,
    RIGHT_IMAGE_PROCESSED = 3,
    DISPARITY_IMAGE       = 4,
    POINT_CLOUD           = 5,
    SEGMENT_RESULT        = 6,
};

struct Point {
    float   x;
    float   y;
    float   z;
    int32_t type;
};

struct PointCloud {
    double             timestamp;
    std::vector<Point> points;
};

} // namespace demo

using demo::DataTypeId;

class AddTenTask final : public Task {
public:
    AddTenTask() : Task("Add10") {
        printf("[AddTenTask] 创建任务 '%s'\n", name().c_str());
    }

    const std::vector<data_type_id_t>& consumes() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::LEFT_IMAGE),
        };
        return v;
    }

    const std::vector<data_type_id_t>& produces() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::LEFT_IMAGE_PROCESSED),
        };
        return v;
    }

    bool run(frame_id_t fid, TaskContext& ctx) override {
        int x = 0;
        if (!ctx.get_data(fid, DataTypeId::LEFT_IMAGE, x)) {
            return false;
        }

        // 失效测试：让第3帧失败，测试后续任务是否被跳过
        if (fid == 3) {
            printf("  [Add10] Failed: frame_id = %u\n", fid);
            return false;
        }

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

    const std::vector<data_type_id_t>& consumes() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::LEFT_IMAGE_PROCESSED),
        };
        return v;
    }

    const std::vector<data_type_id_t>& produces() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::DISPARITY_IMAGE),
        };
        return v;
    }

    bool run(frame_id_t fid, TaskContext& ctx) override {
        int x = 0;
        if (!ctx.get_data(fid, DataTypeId::LEFT_IMAGE_PROCESSED, x)) {
            return false;
        }

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

    const std::vector<data_type_id_t>& consumes() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::DISPARITY_IMAGE),
        };
        return v;
    }

    const std::vector<data_type_id_t>& produces() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::POINT_CLOUD),
        };
        return v;
    }

    bool run(frame_id_t fid, TaskContext& ctx) override {
        int result = 0;
        if (!ctx.get_data(fid, DataTypeId::DISPARITY_IMAGE, result)) {
            return false;
        }

        // 演示项目自定义的数据类型
        demo::PointCloud point_cloud;
        point_cloud.timestamp = fid;
        point_cloud.points.push_back(demo::Point{1.0f, 2.0f, 3.0f, 0});
        ctx.set_data(fid, DataTypeId::POINT_CLOUD, point_cloud);

        printf("  [PrintResult] Success: frame_id = %u, result = %d\n", fid, result);
        return true;
    }
};

class DemoFlow final : public TaskFlow {
public:
    explicit DemoFlow() : TaskFlow("DemoFlow") {
        printf("[DemoFlow] >>> 开始创建任务流 '%s'\n", name().c_str());
        register_data<int>(DataTypeId::LEFT_IMAGE);
        register_data<int>(DataTypeId::LEFT_IMAGE_PROCESSED);
        register_data<int>(DataTypeId::DISPARITY_IMAGE);
        register_data<demo::PointCloud>(DataTypeId::POINT_CLOUD);

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
