#pragma once

#include <any>
#include <cstdio>
#include <map>
#include <memory>
#include <utility>

#include "DataBuffer.h"
#include "Type.h"

namespace scheduler {

class DataHub {
public:
    DataHub() = default;
    ~DataHub() = default;

    DataHub(const DataHub&) = delete;
    DataHub& operator=(const DataHub&) = delete;
    DataHub(DataHub&&) = delete;
    DataHub& operator=(DataHub&&) = delete;

    template <typename DataType>
    bool add_data_buffer(const data_meta_info_t& data_type_info) {
        auto it = data_buffer_map_.find(data_type_info.data_type_id);
        if (it != data_buffer_map_.end()) {
            printf("DataHub: data_type_id `%u` already exists\n",
                   data_type_info.data_type_id);
            return false;
        }
        auto buf = std::make_shared<DataBuffer<DataType>>(
            data_type_info.capacity, data_type_info.elem_size, data_type_info.copy_func);
        data_buffer_map_[data_type_info.data_type_id] = std::any(std::move(buf));
        return true;
    }

    template <typename DataType>
    bool push_data(data_type_id_t data_type_id, frame_id_t frame_id, const DataType& data) {
        DataBuffer<DataType>* buffer = get_data_buffer<DataType>(data_type_id);
        if (buffer == nullptr) {
            return false;
        }
        printf("[数据仓库] 存入第 %u 帧数据, 数据类型 %u\n", frame_id, data_type_id);
        return buffer->push(frame_id, data);
    }

    template <typename DataType>
    DataBuffer<DataType>* get_data_buffer(data_type_id_t data_type_id) {
        auto it = data_buffer_map_.find(data_type_id);
        if (it == data_buffer_map_.end()) {
            printf("DataHub: data_type_id `%u` not found\n", data_type_id);
            return nullptr;
        }
        // 指针形式 any_cast：类型不匹配返回 nullptr；值/引用形式会抛 bad_any_cast。
        auto* holder = std::any_cast<std::shared_ptr<DataBuffer<DataType>>>(&it->second);
        if (holder == nullptr) {
            printf("DataHub: data_type_id `%u` type mismatch\n", data_type_id);
            return nullptr;
        }
        return holder->get();
    }

    template <typename DataType>
    bool get_data(data_type_id_t data_type_id, frame_id_t frame_id, DataType& data) {
        DataBuffer<DataType>* buffer = get_data_buffer<DataType>(data_type_id);
        if (buffer == nullptr) {
            return false;
        }
        return buffer->get_by_id(frame_id, data);
    }

private:
    std::map<data_type_id_t, std::any> data_buffer_map_;
};

} // namespace scheduler
