#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "scheduler/Scheduler.h"
#include "scheduler/Task.h"
#include "scheduler/TaskContext.h"
#include "scheduler/Type.h"

using scheduler::data_type_id_t;
using scheduler::frame_id_t;
using scheduler::Scheduler;
using scheduler::Task;
using scheduler::TaskContext;
using scheduler::TaskFlow;
using scheduler::TaskStatus;

namespace demo {

enum class DataTypeId : data_type_id_t {
    X = 1,
    Y = 2,
    Z = 3,
    A = 4,
    B = 5,
};

} // namespace demo

using demo::DataTypeId;

class XPlus2EqualsYTask final : public Task {
public:
    XPlus2EqualsYTask() : Task("X+2=Y") {}

    const std::vector<data_type_id_t>& consumes() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::X),
        };
        return v;
    }

    const std::vector<data_type_id_t>& produces() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::Y),
        };
        return v;
    }

    auto run(frame_id_t frame_id, TaskContext& ctx) -> TaskStatus override {
        auto x_lease = ctx.read_data_lease(frame_id, static_cast<data_type_id_t>(DataTypeId::X));
        if (!x_lease.ok()) {
            return TaskStatus::FAILED;
        }
        const int X = *x_lease.as<int>();

        // 失效测试：让第3帧失败，测试后续任务是否被跳过
        if (frame_id == 3) {
            printf("%s: Failed: frame_id = %u\n", name().c_str(), frame_id);
            return TaskStatus::FAILED;
        }

        int Y = X + 2;
        printf("%s: Success: frame_id = %u, input = %d, output = %d\n", name().c_str(), frame_id, X, Y);

        ctx.write_data(frame_id, static_cast<data_type_id_t>(DataTypeId::Y), &Y);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return TaskStatus::COMPLETED;
    }
};

class YTimes2EqualsZTask final : public Task {
public:
    YTimes2EqualsZTask() : Task("Y*2=Z") {}

    const std::vector<data_type_id_t>& consumes() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::Y),
        };
        return v;
    }

    const std::vector<data_type_id_t>& produces() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::Z),
        };
        return v;
    }

    auto run(frame_id_t frame_id, TaskContext& ctx) -> TaskStatus override {
        auto y_lease = ctx.read_data_lease(frame_id, static_cast<data_type_id_t>(DataTypeId::Y));
        if (!y_lease.ok()) {
            return TaskStatus::FAILED;
        }
        const int Y = *y_lease.as<int>();

        int Z = Y * 2;
        printf("%s: Success: frame_id = %u, input = %d, output = %d\n", name().c_str(), frame_id, Y, Z);

        ctx.write_data(frame_id, static_cast<data_type_id_t>(DataTypeId::Z), &Z);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return TaskStatus::COMPLETED;
    }
};

class ZPlus3EqualsATask final : public Task {
public:
    explicit ZPlus3EqualsATask() : Task("Z+3=A") {}

    const std::vector<data_type_id_t>& consumes() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::Z),
        };
        return v;
    }

    const std::vector<data_type_id_t>& produces() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::A),
        };
        return v;
    }

    auto run(frame_id_t frame_id, TaskContext& ctx) -> TaskStatus override {
        auto z_lease = ctx.read_data_lease(frame_id, static_cast<data_type_id_t>(DataTypeId::Z));
        if (!z_lease.ok()) {
            return TaskStatus::FAILED;
        }
        const int Z = *z_lease.as<int>();

        int A = Z + 3;
        ctx.write_data(frame_id, static_cast<data_type_id_t>(DataTypeId::A), &A);

        printf("%s: Success: frame_id = %u, input = %d, output = %d\n", name().c_str(), frame_id, Z, A);
        return TaskStatus::COMPLETED;
    }
};

class XPlusYEqualsBTask final : public Task {
public:
    explicit XPlusYEqualsBTask() : Task("X+Y=B") {}

    const std::vector<data_type_id_t>& consumes() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::X),
            static_cast<data_type_id_t>(DataTypeId::Y),
        };
        return v;
    }

    const std::vector<data_type_id_t>& produces() const override {
        static const std::vector<data_type_id_t> v = {
            static_cast<data_type_id_t>(DataTypeId::B),
        };
        return v;
    }

    auto run(frame_id_t frame_id, TaskContext& ctx) -> TaskStatus override {
        auto x_lease = ctx.read_data_lease(frame_id, static_cast<data_type_id_t>(DataTypeId::X));
        if (!x_lease.ok()) {
            return TaskStatus::FAILED;
        }
        const int X = *x_lease.as<int>();

        auto y_lease = ctx.read_data_lease(frame_id, static_cast<data_type_id_t>(DataTypeId::Y));
        if (!y_lease.ok()) {
            return TaskStatus::FAILED;
        }
        const int Y = *y_lease.as<int>();

        int B = X + Y;
        ctx.write_data(frame_id, static_cast<data_type_id_t>(DataTypeId::B), &B);

        printf("%s: Success: frame_id = %u, input1 = %d, input2 = %d, output = %d\n", name().c_str(), frame_id, X, Y, B);
        return TaskStatus::COMPLETED;
    }
};

class CalculateA final : public TaskFlow {
public:
    explicit CalculateA() : TaskFlow("CalculateA") {
        add_data_type(static_cast<data_type_id_t>(DataTypeId::X), "X", sizeof(int), 10);
        add_data_type(static_cast<data_type_id_t>(DataTypeId::Y), "Y", sizeof(int), 10);
        add_data_type(static_cast<data_type_id_t>(DataTypeId::Z), "Z", sizeof(int), 10);
        add_data_type(static_cast<data_type_id_t>(DataTypeId::A), "A", sizeof(int), 10);

        add_task(std::make_unique<XPlus2EqualsYTask>());
        add_task(std::make_unique<YTimes2EqualsZTask>());
        add_task(std::make_unique<ZPlus3EqualsATask>());
    }
};

class CalculateB final : public TaskFlow {
public:
    explicit CalculateB() : TaskFlow("CalculateB") {
        add_data_type(static_cast<data_type_id_t>(DataTypeId::X), "X", sizeof(int), 10);
        add_data_type(static_cast<data_type_id_t>(DataTypeId::Y), "Y", sizeof(int), 10);
        add_data_type(static_cast<data_type_id_t>(DataTypeId::B), "B", sizeof(int), 10);

        add_task(std::make_unique<XPlusYEqualsBTask>());
    }
};

int main() {
    // X + 2 = Y
    // Y * 2 = Z
    // Z + 3 = A
    // X + Y = B
    // print A and B
    Scheduler scheduler;
    scheduler.register_task_flow(std::make_unique<CalculateA>());
    scheduler.register_task_flow(std::make_unique<CalculateB>());

    if (!scheduler.start()) {
        printf("Scheduler start failed\n");
        return 1;
    }

    for (frame_id_t id = 1; id <= 5; ++id) {
        int input = static_cast<int>(id) * 2;
        const auto x_id = static_cast<data_type_id_t>(DataTypeId::X);
        printf("Feed data type %u, frame %u, value %d\n", x_id, id, input);
        if (!scheduler.feed(id, x_id, &input)) {
            printf("Failed to feed data type %u, frame %u, value %d\n", x_id, id, input);
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(20));
    scheduler.stop();
    return 0;
}
