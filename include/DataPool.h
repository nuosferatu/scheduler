#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

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
    size_t                 elem_size_;  // 数据元素大小
};

} // namespace scheduler
