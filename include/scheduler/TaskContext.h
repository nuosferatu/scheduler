#pragma once

#include <functional>
#include <utility>

#include "scheduler/DataHub.h"
#include "scheduler/Type.h"

namespace scheduler {

class TaskContext {
public:
    using release_priority_callback_t = std::function<void()>;

    TaskContext(DataHub& data_hub,
                bool requires_priority_lock = false,
                release_priority_callback_t release_priority_callback = nullptr)
        : data_hub_(data_hub),
          requires_priority_lock_(requires_priority_lock),
          priority_lock_held_(false),
          release_priority_callback_(release_priority_callback ? std::move(release_priority_callback) : nullptr) {}

    ~TaskContext() = default;

    TaskContext(const TaskContext&)            = delete;
    TaskContext& operator=(const TaskContext&) = delete;
    TaskContext(TaskContext&&)                 = delete;
    TaskContext& operator=(TaskContext&&)      = delete;

    // Read data
    auto read_data_lease(frame_id_t     frame_id,
                         data_type_id_t data_type_id) -> DataBuffer::ReadLease {
        return data_hub_.read_data_lease(data_type_id, frame_id);
    }

    // Copy-write data
    auto write_data(frame_id_t     frame_id,
                    data_type_id_t data_type_id,
                    const void*    data) -> data_access_status_t {
        return data_hub_.write_data(data_type_id, frame_id, data);
    }

    // Acquire a memory slot and write data directly;
    // if growth is needed, call ensure_capacity(N) on the returned lease.
    auto write_data_lease(frame_id_t     frame_id,
                          data_type_id_t data_type_id) -> DataBuffer::WriteLease {
        return data_hub_.write_data_lease(data_type_id, frame_id);
    }

    // For Worker to release priority lock
    //  1. called manually during task execution (optional)
    //  2. called automatically when the task is completed
    void release_priority_lock() {
        if (!requires_priority_lock_ || !priority_lock_held_) {
            return;
        }
        if (release_priority_callback_) {
            release_priority_callback_();
        }
        priority_lock_held_ = false;
    }

    // For Worker to hold priority lock
    //  called automatically before task execution (only if priority is required)
    void hold_priority_lock() {
        if (requires_priority_lock_) {
            priority_lock_held_ = true;
        }
    }

private:
    DataHub&                    data_hub_;
    bool                        requires_priority_lock_;
    bool                        priority_lock_held_;
    release_priority_callback_t release_priority_callback_;
};

} // namespace scheduler
