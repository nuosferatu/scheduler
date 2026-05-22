#pragma once

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "scheduler/DataBuffer.h"
#include "scheduler/Type.h"

namespace scheduler {

class DataHub {
public:
    DataHub()  = default;
    ~DataHub() = default;

    DataHub(const DataHub&)            = delete;
    DataHub& operator=(const DataHub&) = delete;
    DataHub(DataHub&&)                 = delete;
    DataHub& operator=(DataHub&&)      = delete;

    auto create_buffer(data_type_id_t      id,
                       size_t              elem_size,
                       size_t              capacity              = 0,
                       copy_func_t         copy_func             = nullptr,
                       release_slot_func_t release_slot_func     = nullptr,
                       init_slot_func_t    init_slot_func        = nullptr,
                       size_t              grow_step             = 0,
                       size_t              write_wait_timeout_ms = 0) -> bool {
        if (is_inited(id)) {
            printf("[DataHub] [Warn] create_buffer: data type %u:%s already initialized\n",
                   id, data_names_[id].c_str());
            return true;
        }
        if (elem_size == 0) {
            printf("[DataHub] [Error] create_buffer: data type %u:%s elem_size is 0\n",
                   id, data_names_[id].c_str());
            return false;
        }

        auto& info = data_meta_map_[id];
        info.data_type_id          = id;
        info.elem_size             = elem_size;
        info.capacity              = capacity > 0 ? capacity : DefaultBufferCapacity;
        info.grow_step             = grow_step;
        info.write_wait_timeout_ms = write_wait_timeout_ms;
        if (copy_func) {
            info.copy_func = std::move(copy_func);
        }
        if (release_slot_func) {
            info.release_slot_func = std::move(release_slot_func);
        }
        if (init_slot_func) {
            info.init_slot_func = std::move(init_slot_func);
        }

        data_buffer_map_[id] = std::make_unique<DataBuffer>(
            info.capacity,
            info.elem_size,
            info.copy_func,
            info.release_slot_func,
            info.init_slot_func,
            data_names_[id],
            info.grow_step,
            info.write_wait_timeout_ms);
        inited_map_[id] = true;
        printf("[DataHub] data type %u:%s initialized, elem_size=%zu, capacity=%zu, "
               "grow_step=%zu, write_wait_timeout_ms=%zu\n",
               id, data_names_[id].c_str(), info.elem_size, info.capacity,
               info.grow_step, info.write_wait_timeout_ms);
        return true;
    }

    auto is_inited(data_type_id_t id) const -> bool {
        auto it = inited_map_.find(id);
        return it != inited_map_.end() && it->second;
    }

    auto get_data_buffer(data_type_id_t id) -> DataBuffer* {
        auto it = data_buffer_map_.find(id);
        if (it == data_buffer_map_.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    auto data_names() -> std::map<data_type_id_t, std::string>& {
        return data_names_;
    }

    /**
     * Three data access APIs:
     * 1. read_data_lease(id, frame)  : read-only access to a frame's data
     * 2. write_data(id, frame, data) : copy-write a frame's data
     * 3. write_data_lease(id, frame) : acquire a memory slot to write a frame's data directly
     */

    auto read_data_lease(data_type_id_t id, frame_id_t frame) -> DataBuffer::ReadLease {
        if (data_meta_map_.count(id) == 0) {
            printf("[DataHub] [Error] read_data_lease: data type %u not found\n", id);
            return DataBuffer::ReadLease(DataAccessStatus::NOT_FOUND);
        }
        if (!is_inited(id)) {
            printf("[DataHub] read_data_lease: data type %u:%s not initialized yet\n",
                   id, data_names_[id].c_str());
            return DataBuffer::ReadLease(DataAccessStatus::NOT_INITIALIZED);
        }
        auto* buffer = get_data_buffer(id);
        if (buffer == nullptr) {
            printf("[DataHub] [Error] read_data_lease: data type %u:%s has no buffer\n",
                   id, data_names_[id].c_str());
            return DataBuffer::ReadLease(DataAccessStatus::INTERNAL_ERROR);
        }
        return buffer->read_lease(frame);
    }

    auto write_data(data_type_id_t id,
                    frame_id_t frame,
                    const void* data) -> data_access_status_t {
        if (data_meta_map_.count(id) == 0) {
            printf("[DataHub] [Error] write_data: data type %u not found\n", id);
            return DataAccessStatus::NOT_FOUND;
        }
        if (!is_inited(id)) {
            printf("[DataHub] write_data: data type %u:%s not initialized yet\n",
                   id, data_names_[id].c_str());
            return DataAccessStatus::NOT_INITIALIZED;
        }
        auto* buffer = get_data_buffer(id);
        if (buffer == nullptr) {
            printf("[DataHub] [Error] write_data: data type %u:%s has no buffer\n",
                   id, data_names_[id].c_str());
            return DataAccessStatus::INTERNAL_ERROR;
        }
        if (!buffer->write(frame, data)) {
            printf("[DataHub] [Error] write_data: data type %u:%s frame %u pool full\n",
                   id, data_names_[id].c_str(), frame);
            return DataAccessStatus::POOL_FULL;
        }
        printf("[DataHub] data type %u:%s, frame %u written\n",
               id, data_names_[id].c_str(), frame);
        return DataAccessStatus::OK;
    }

    void dump_all_pinned() {
        // printf("[DataHub] [Dump] ===== pinned slots across all buffers =====\n");
        // for (auto& [id, buffer] : data_buffer_map_) {
        //     if (buffer == nullptr) {
        //         continue;
        //     }
        //     buffer->dump_pinned();
        // }
        // printf("[DataHub] [Dump] ===== end pinned slots =====\n");
    }

    auto write_data_lease(data_type_id_t id,
                          frame_id_t frame) -> DataBuffer::WriteLease {
        if (data_meta_map_.count(id) == 0) {
            printf("[DataHub] [Error] write_data_lease: data type %u not found\n", id);
            return DataBuffer::WriteLease(DataAccessStatus::NOT_FOUND);
        }
        if (!is_inited(id)) {
            printf("[DataHub] write_data_lease: data type %u:%s not initialized yet\n",
                   id, data_names_[id].c_str());
            return DataBuffer::WriteLease(DataAccessStatus::NOT_INITIALIZED);
        }
        auto* buffer = get_data_buffer(id);
        if (buffer == nullptr) {
            printf("[DataHub] [Error] write_data_lease: data type %u:%s has no buffer\n",
                   id, data_names_[id].c_str());
            return DataBuffer::WriteLease(DataAccessStatus::INTERNAL_ERROR);
        }
        return buffer->write_lease(frame);
    }

private:
    std::map<data_type_id_t, data_meta_info_t>            data_meta_map_;
    std::map<data_type_id_t, std::unique_ptr<DataBuffer>> data_buffer_map_;
    std::map<data_type_id_t, bool>                        inited_map_;
    std::map<data_type_id_t, std::string>                 data_names_;
};

} // namespace scheduler
