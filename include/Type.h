#pragma once

#include <bits/stdint-intn.h>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace scheduler {

using frame_id_t = uint32_t;
using copy_func_t = std::function<void(void* dst, const void* src)>;

constexpr size_t DefaultBufferCapacity = 16;

enum class TaskStatus : uint32_t {
    INITIALIZED = 0,
    PENDING     = 1,  // 在帧实例中等待输入
    READY       = 2,  // 输入已齐, 等待被分发到 Worker
    RUNNING     = 3,  // 已分发到 Worker
    COMPLETED   = 4,
    FAILED      = 5,
    SKIPPED     = 6,  // 因传递性上游失败被跳过
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

using data_meta_info_t = struct DataMetaInfo {
    DataTypeId  data_type_id;
    size_t      capacity;
    size_t      elem_size;
    copy_func_t copy_func;
};

using point_t = struct Point {
    float x;
    float y;
    float z;
    int32_t type;
};

using point_cloud_t = struct PointCloud {
    double timestamp;
    std::vector<point_t> points;
};

} // namespace scheduler
