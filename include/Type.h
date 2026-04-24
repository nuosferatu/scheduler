#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace scheduler {

class TaskContext;

using frame_id_t = uint32_t;
using task_node_id_t = uint32_t;
using copy_func_t = std::function<void(void* dst, const void* src)>;
using task_func_t = std::function<bool(TaskContext&)>;

enum class TaskStatus : uint32_t {
    INITIALIZED = 0,
    PENDING     = 1,
    READY       = 2,
    RUNNING     = 3,
    COMPLETED   = 4,
    FAILED      = 5,
};

enum class DataTypeId : uint32_t {
    LEFT_IMAGE            = 0,
    RIGHT_IMAGE           = 1,
    LEFT_IMAGE_PROCESSED  = 2,
    RIGHT_IMAGE_PROCESSED = 3,
    DISPARITY_IMAGE       = 4,
    POINT_CLOUD           = 5,
    SEGMENT_RESULT        = 6,
};

using data_type_info_t = struct DataTypeInfo {
    DataTypeId  data_type_id;
    size_t      capacity;
    size_t      elem_size;
    copy_func_t copy_func;
};

} // namespace scheduler
