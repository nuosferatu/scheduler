#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <shared_mutex>
#include <utility>

#include "DataPool.h"
#include "Type.h"

namespace scheduler {

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

        // 同 frame_id 重复 push：先回收旧块
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
            pop_locked();
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
        return pop_locked();
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
            printf("DataBuffer: id %u, data: %p, size: %zu\n", id, it->second, elem_size_);
        }
    }

private:
    bool pop_locked() {
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
    std::deque<frame_id_t>          buffer_;     // 数据缓冲区
    std::map<frame_id_t, DataType*> id_vs_ptr_;
    std::shared_mutex               mutex_;      // 互斥锁
    size_t                          capacity_;
    size_t                          elem_size_;
    copy_func_t                     copy_func_;
};

} // namespace scheduler
