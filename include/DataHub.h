#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <any>
#include <vector>
#include <map>
#include <mutex>
#include <deque>
#include <memory>
#include <utility>
#include <shared_mutex>

#include "Type.h"

namespace scheduler {

template <typename DataType>
class DataPool {
public:
    DataPool(size_t pool_size, size_t elem_size) : elem_size_(elem_size) {
        pool_.resize(pool_size);
        free_list_.resize(pool_size);
        for (size_t i = 0; i < pool_size; ++i) {
            auto ptr = static_cast<DataType*>(std::malloc(elem_size_));
            std::memset(ptr, 0, elem_size_);
            pool_[i] = ptr;
            free_list_[i] = ptr;
        }
    }

    ~DataPool() {
        for (auto ptr : pool_) {
            std::free(ptr);
        }
    }

    // 禁止拷贝/移动：独占内存池与 mutex，拷贝会破坏所有权。
    DataPool(const DataPool&) = delete;
    DataPool& operator=(const DataPool&) = delete;
    DataPool(DataPool&&) = delete;
    DataPool& operator=(DataPool&&) = delete;

    DataType* acquire_ptr() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_list_.empty()) {
            return nullptr;
        }
        auto* ptr = free_list_.front();
        free_list_.pop_front();
        return ptr;
    }

    void release_ptr(DataType* ptr) {
        if (ptr == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        std::memset(ptr, 0, elem_size_);
        free_list_.push_back(ptr);
    }

private:
    std::vector<DataType*> pool_;       // 内存池
    std::deque<DataType*>  free_list_;  // 内存池空闲列表
    std::mutex             mutex_;      // 互斥锁
    size_t                 elem_size_;
};

template <typename DataType>
class DataBuffer {
public:
    DataBuffer(size_t capacity,
               size_t elem_size,
               copy_func_t copy_func)
        : pool_(capacity * 2, elem_size),
          capacity_(capacity),
          elem_size_(elem_size),
          copy_func_(std::move(copy_func)) {
    }

    ~DataBuffer() = default;

    // 禁止拷贝/移动：管理池指针与 shared_mutex，不可复制。
    DataBuffer(const DataBuffer&) = delete;
    DataBuffer& operator=(const DataBuffer&) = delete;
    DataBuffer(DataBuffer&&) = delete;
    DataBuffer& operator=(DataBuffer&&) = delete;

    size_t size() {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return buffer_.size();
    }

    bool push(frame_id_t id, const DataType& data) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        // 同 frame_id 重复 push：先回收旧块，避免池泄漏与 buffer/map 不一致。
        auto existing = id_vs_ptr_.find(id);
        if (existing != id_vs_ptr_.end()) {
            for (auto it = buffer_.begin(); it != buffer_.end(); ++it) {
                if (*it == id) {
                    buffer_.erase(it);
                    break;
                }
            }
            pool_.release_ptr(existing->second);
            id_vs_ptr_.erase(existing);
        }

        if (buffer_.size() >= capacity_) {
            pop_unlocked();
        }

        auto ptr = pool_.acquire_ptr();
        if (ptr == nullptr) {
            printf("DataBuffer: data pool is full\n");
            return false;
        }

        if (copy_func_) {
            copy_func_(ptr, &data);
        } else {
            std::memcpy(ptr, &data, elem_size_);
        }

        buffer_.push_back(id);
        id_vs_ptr_[id] = ptr;
        return true;
    }

    bool pop() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return pop_unlocked();
    }

    bool pop(DataType& data) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (buffer_.empty()) {
            printf("DataBuffer: buffer is empty\n");
            return false;
        }

        auto id = buffer_.front();
        auto it = id_vs_ptr_.find(id);
        if (it == id_vs_ptr_.end()) {
            printf("DataBuffer: id %u not found\n", id);
            return false;
        }

        auto ptr = it->second;
        if (copy_func_) {
            copy_func_(&data, ptr);
        } else {
            std::memcpy(&data, ptr, elem_size_);
        }

        buffer_.pop_front();
        pool_.release_ptr(ptr);
        id_vs_ptr_.erase(it);
        return true;
    }

    bool get_by_id(frame_id_t id, DataType& data) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = id_vs_ptr_.find(id);
        if (it == id_vs_ptr_.end()) {
            printf("DataBuffer: id %u not found\n", id);
            return false;
        }
        auto ptr = it->second;
        if (copy_func_) {
            copy_func_(&data, ptr);
        } else {
            std::memcpy(&data, ptr, elem_size_);
        }
        return true;
    }

    void print_buffer() {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (auto id : buffer_) {
            auto it = id_vs_ptr_.find(id);
            if (it == id_vs_ptr_.end()) {
                printf("DataBuffer: id %u not found\n", id);
                continue;
            }
            std::cout << "id: " << id << ", data: " << *it->second << std::endl;
            std::cout << "--------------------------------" << std::endl;
        }
    }

private:
    // 调用方必须已持有 mutex_ 的独占锁。
    bool pop_unlocked() {
        if (buffer_.empty()) {
            printf("DataBuffer: buffer is empty\n");
            return false;
        }

        auto id = buffer_.front();
        auto it = id_vs_ptr_.find(id);
        if (it == id_vs_ptr_.end()) {
            printf("DataBuffer: id %u not found\n", id);
            buffer_.pop_front();
            return false;
        }

        buffer_.pop_front();
        pool_.release_ptr(it->second);
        id_vs_ptr_.erase(it);
        return true;
    }

    DataPool<DataType>              pool_;
    std::deque<frame_id_t>          buffer_;      // 数据缓冲区
    std::map<frame_id_t, DataType*> id_vs_ptr_;
    std::shared_mutex               mutex_;       // 互斥锁
    size_t                          capacity_;
    size_t                          elem_size_;
    copy_func_t                     copy_func_;
};

class DataHub {
public:
    DataHub() = default;
    ~DataHub() = default;

    // 禁止拷贝/移动：中心注册表含任意类型缓冲区，复制无定义良好的语义。
    DataHub(const DataHub&) = delete;
    DataHub& operator=(const DataHub&) = delete;
    DataHub(DataHub&&) = delete;
    DataHub& operator=(DataHub&&) = delete;

    template <typename DataType>
    bool add_data_buffer(const data_meta_info_t& data_type_info) {
        auto it = data_buffer_map_.find(data_type_info.data_type_id);
        if (it != data_buffer_map_.end()) {
            printf("DataHub: data_type_id `%d` already exists\n",
                   static_cast<int32_t>(data_type_info.data_type_id));
            return false;
        }
        auto buf = std::make_shared<DataBuffer<DataType>>(
            data_type_info.capacity, data_type_info.elem_size, data_type_info.copy_func);
        data_buffer_map_[data_type_info.data_type_id] = std::any(std::move(buf));
        return true;
    }

    template <typename DataType>
    bool push_data(const DataTypeId& data_type_id, frame_id_t frame_id, const DataType& data) {
        DataBuffer<DataType>* buffer = get_data_buffer<DataType>(data_type_id);
        if (buffer == nullptr) {
            return false;
        }
        return buffer->push(frame_id, data);
    }

    template <typename DataType>
    DataBuffer<DataType>* get_data_buffer(const DataTypeId& data_type_id) {
        auto it = data_buffer_map_.find(data_type_id);
        if (it == data_buffer_map_.end()) {
            printf("DataHub: data_type_id `%d` not found\n",
                   static_cast<int32_t>(data_type_id));
            return nullptr;
        }
        // 指针形式 any_cast：类型不匹配返回 nullptr；值/引用形式会抛 bad_any_cast。
        auto* holder = std::any_cast<std::shared_ptr<DataBuffer<DataType>>>(&it->second);
        if (holder == nullptr) {
            printf("DataHub: data_type_id `%d` type mismatch\n",
                   static_cast<int32_t>(data_type_id));
            return nullptr;
        }
        return holder->get();
    }

    template <typename DataType>
    bool get_data(const DataTypeId& data_type_id, frame_id_t frame_id, DataType& data) {
        DataBuffer<DataType>* buffer = get_data_buffer<DataType>(data_type_id);
        if (buffer == nullptr) {
            return false;
        }
        return buffer->get_by_id(frame_id, data);
    }

private:
    std::map<DataTypeId, std::any> data_buffer_map_;
};

} // namespace scheduler
