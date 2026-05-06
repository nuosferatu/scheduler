#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <utility>
#include <vector>

#include "DataHub.h"
#include "Type.h"

namespace scheduler {

class TaskContext {
public:
    using release_priority_cb_t = std::function<void()>;

    explicit TaskContext(DataHub& data_hub,
                         bool requires_priority_lock = false,
                         release_priority_cb_t release_cb = nullptr)
        : data_hub_(data_hub),
          requires_priority_lock_(requires_priority_lock),
          release_priority_cb_(std::move(release_cb)),
          priority_held_(false) {}

    ~TaskContext() = default;

    TaskContext(const TaskContext&) = delete;
    TaskContext& operator=(const TaskContext&) = delete;
    TaskContext(TaskContext&&) = delete;
    TaskContext& operator=(TaskContext&&) = delete;

    template <typename T, typename Id>
    bool get_data(frame_id_t frame_id, Id data_type_id, T& data) {
        return data_hub_.get_data(static_cast<data_type_id_t>(data_type_id), frame_id, data);
    }

    template <typename T, typename Id>
    bool set_data(frame_id_t frame_id, Id data_type_id, const T& data) {
        return data_hub_.push_data(static_cast<data_type_id_t>(data_type_id), frame_id, data);
    }

    void release_priority_lock() {
        if (!priority_held_) {
            return;
        }
        if (release_priority_cb_) {
            release_priority_cb_();
        }
        priority_held_ = false;
    }

    void hold_priority_lock() {
        priority_held_ = requires_priority_lock_;
    }

private:
    DataHub&              data_hub_;
    release_priority_cb_t release_priority_cb_;
    bool                  requires_priority_lock_;
    bool                  priority_held_;
};

} // namespace scheduler
