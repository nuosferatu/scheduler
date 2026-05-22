#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace scheduler {

using frame_id_t     = uint32_t;
using data_type_id_t = uint32_t;
using copy_func_t    = std::function<void(void* dst, const void* src)>;
using release_slot_func_t = std::function<void(void* ptr)>;
// memory_size is used for expansion
using init_slot_func_t = std::function<void(void* ptr, size_t memory_size)>;

constexpr size_t DefaultBufferCapacity = 16;

// Limit of thread name length
constexpr size_t kMaxThreadNameLen = 15;

// Check thread name length in compile time
template <size_t N>
constexpr auto thread_name_literal(const char (&literal)[N]) -> const char* {
    static_assert(N <= kMaxThreadNameLen + 1,
                  "scheduler thread name must be <= 15 characters (excluding null terminator)");
    return literal;
}

// Fallback for thread name if Task::thread_name() is not provided
inline auto truncate_thread_name(std::string name) -> std::string {
    if (name.size() > kMaxThreadNameLen) {
        name.resize(kMaxThreadNameLen);
    }
    return name;
}

enum class TaskStatus : uint32_t {
    // INITIALIZED = 0,
    PENDING   = 1,  // waiting for inputs
    READY     = 2,  // inputs are ready, to be dispatched to Worker
    RUNNING   = 3,  // dispatched to Worker
    COMPLETED = 4,  // task completed
    FAILED    = 5,  // task failed
    SKIPPED   = 6,  // task skipped due to failed dependencies
    RETRY     = 7,  // transient: run could not proceed; scheduler resets to PENDING
};

// Per-frame scheduling state
struct FrameInstance {
    std::map<std::string, TaskStatus> task_status;  // task status
    std::set<data_type_id_t>          arrived;      // arrived data types
    std::set<data_type_id_t>          failed;       // failed data types
    size_t                            finished = 0; // completed tasks count
};

enum class DataAccessStatus : uint8_t {
    OK              = 0,
    NOT_FOUND       = 1,
    NOT_INITIALIZED = 2,
    FRAME_NOT_FOUND = 3,
    POOL_FULL       = 4,
    INTERNAL_ERROR  = 5,
};
using data_access_status_t = DataAccessStatus;

struct DataMetaInfo {
    data_type_id_t      data_type_id          = 0;
    size_t              capacity              = 0;
    size_t              elem_size             = 0;
    size_t              grow_step             = 0;  // slot grow step
    size_t              write_wait_timeout_ms = 0;  // see DataTypeDescriptor::write_wait_timeout_ms
    copy_func_t         copy_func;
    release_slot_func_t release_slot_func;
    init_slot_func_t    init_slot_func;
};
using data_meta_info_t = DataMetaInfo;

} // namespace scheduler
