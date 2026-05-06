#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace scheduler {

using frame_id_t       = uint32_t;
using data_type_id_t   = uint32_t;
using copy_func_t      = std::function<void(void* dst, const void* src)>;

constexpr size_t DefaultBufferCapacity = 16;

enum class TaskStatus : uint32_t {
    // INITIALIZED = 0,
    PENDING     = 1,  // 在帧实例中等待输入
    READY       = 2,  // 输入已齐, 等待被分发到 Worker 执行
    RUNNING     = 3,  // 已分发到 Worker 执行
    COMPLETED   = 4,  // 任务完成
    FAILED      = 5,  // 任务失败
    SKIPPED     = 6,  // 因前置任务失败而被跳过
};

using data_meta_info_t = struct DataMetaInfo {
    data_type_id_t data_type_id;
    size_t         capacity;
    size_t         elem_size;
    copy_func_t    copy_func;
};

} // namespace scheduler
