#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scheduler/TaskContext.h"
#include "scheduler/Type.h"

namespace scheduler {

class TaskFlow;

class Task {
    friend class TaskFlow;

public:
    Task(const std::string& name) : name_(name) {}
    virtual ~Task() = default;

    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    auto name() const -> const std::string& { return name_; }

    /**
     * Virtual functions: Specific tasks can override (optional)
     */

    // Priority
    //  0 means no priority
    //  1+ means priority, 1 is the highest priority
    virtual auto priority() const -> uint8_t { return 0; }

    // Init task context
    //  called once when Worker starts
    virtual auto init(TaskContext& ctx) -> bool { return true; }

    // Cleanup task context
    //  called once when Worker stops
    virtual void stop(TaskContext& ctx) {}

    // Thread name (Linux pthread/prctl limit is <= 15 characters).
    //   Returning nullptr means unspecified; the Worker falls back to a truncated task name.
    //   Subclasses are encouraged to use scheduler::thread_name_literal("xxx")
    //   to get a compile-time length check.
    virtual auto thread_name() const -> const char* { return nullptr; }

    virtual auto is_ready(const FrameInstance& fi, frame_id_t frame_id, TaskContext& ctx) -> bool {
        (void)frame_id;
        (void)ctx;
        for (auto id : consumes()) {
            if (fi.arrived.count(id) == 0) {
                return false;
            }
        }
        return true;
    }

    /**
     * Interface functions: Specific tasks must implement
     */

    // Data types that this task consumes
    virtual auto consumes() const -> const std::vector<data_type_id_t>& = 0;

    // Data types that this task produces
    virtual auto produces() const -> const std::vector<data_type_id_t>& = 0;

    // Task execution function
    //  called every frame by Worker
    virtual auto run(frame_id_t frame_id, TaskContext& ctx) -> TaskStatus = 0;

protected:
    auto flow() const -> TaskFlow* { return owner_; }

    template <typename T>
    auto flow_as() const -> T* {
        return static_cast<T*>(owner_);
    }

private:
    std::string name_;
    TaskFlow*   owner_ = nullptr;
};

} // namespace scheduler
