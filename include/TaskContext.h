#pragma once

#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

#include "Type.h"
#include "DataHub.h"

namespace scheduler {

class TaskContext {
public:
    TaskContext(DataHub& data_hub) : data_hub_(data_hub) {}

    ~TaskContext() = default;

    // 含 DataHub& 引用成员：引用不可重新绑定，赋值由编译器隐式删除；
    // 这里显式保留默认拷贝构造，便于按值持有到 Worker 中。
    // 注意：不要显式 = delete 任何 move 操作，否则会把隐式拷贝构造连带删掉。
    TaskContext(const TaskContext&) = default;

    template <typename T>
    bool get_data(frame_id_t frame_id, DataTypeId data_type_id, T& data) {
        return data_hub_.get_data(data_type_id, frame_id, data);
    }

    template <typename T>
    bool set_data(frame_id_t frame_id, DataTypeId data_type_id, const T& data) {
        return data_hub_.push_data(data_type_id, frame_id, data);
    }

private:
    DataHub&             data_hub_;
};

} // namespace scheduler
