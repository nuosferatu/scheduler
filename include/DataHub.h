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

        // 若相同 id 已存在，先把旧的回收掉，避免内存池泄漏和 deque/map 不一致。
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
    // Caller must hold the exclusive lock of mutex_
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

    DataHub(const DataHub&) = delete;
    DataHub& operator=(const DataHub&) = delete;
    DataHub(DataHub&&) = delete;
    DataHub& operator=(DataHub&&) = delete;

    template <typename DataType>
    void add_data_buffer(const data_type_info_t& data_type_info) {
        auto it = data_buffer_map_.find(data_type_info.data_type_id);
        if (it != data_buffer_map_.end()) {
            printf("DataHub: data_type_id `%d` already exists\n",
                   static_cast<int32_t>(data_type_info.data_type_id));
            return;
        }
        auto buf = std::make_shared<DataBuffer<DataType>>(
            data_type_info.capacity, data_type_info.elem_size, data_type_info.copy_func);
        data_buffer_map_[data_type_info.data_type_id] = std::any(std::move(buf));
    }

    template <typename DataType>
    DataBuffer<DataType>* get_data_buffer(const DataTypeId& data_type_id) {
        auto it = data_buffer_map_.find(data_type_id);
        if (it == data_buffer_map_.end()) {
            printf("DataHub: data_type_id `%d` not found\n",
                   static_cast<int32_t>(data_type_id));
            return nullptr;
        }
        // std::any_cast 以指针形式调用在类型不匹配时返回 nullptr，
        // 以值/引用形式调用则会抛 std::bad_any_cast，因此这里用指针形式。
        auto* holder = std::any_cast<std::shared_ptr<DataBuffer<DataType>>>(&it->second);
        if (holder == nullptr) {
            printf("DataHub: data_type_id `%d` type mismatch\n",
                   static_cast<int32_t>(data_type_id));
            return nullptr;
        }
        return holder->get();
    }

private:
    std::map<DataTypeId, std::any> data_buffer_map_;
};

} // namespace scheduler
