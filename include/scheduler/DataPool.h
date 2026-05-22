#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace scheduler {

class DataPool {
public:
    DataPool(size_t pool_size, size_t elem_size) : elem_size_(elem_size) {
        for (size_t i = 0; i < pool_size; ++i) {
            auto* ptr = alloc_slot(elem_size_);
            if (ptr != nullptr) {
                free_list_.push_back(ptr);
            }
        }
    }

    ~DataPool() {
        for (auto* ptr : all_ptrs_) {
            std::free(ptr);
        }
    }

    DataPool(const DataPool&)            = delete;
    DataPool& operator=(const DataPool&) = delete;
    DataPool(DataPool&&)                 = delete;
    DataPool& operator=(DataPool&&)      = delete;

    // 取一个空闲槽位，按构造时 elem_size_ 分配/复用。槽位扩展通过 grow_slot 完成。
    auto acquire_ptr() -> void* {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_list_.empty()) {
            return nullptr;
        }
        auto* ptr = free_list_.front();
        free_list_.pop_front();

        auto it = ptr_sizes_.find(ptr);
        if (it != ptr_sizes_.end()) {
            std::memset(ptr, 0, it->second);
        }
        return ptr;
    }

    void release_ptr(void* ptr) {
        if (ptr == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = static_cast<uint8_t*>(ptr);
        auto it = ptr_sizes_.find(slot);
        if (it == ptr_sizes_.end()) {
            return;
        }
        std::memset(slot, 0, it->second);
        free_list_.push_back(slot);
    }

    // 将 ptr 指向的槽位扩展到至少 min_size 字节。
    //   step：扩展步长，0 表示按 min_size 精确分配；>0 时按 ceil 取整到 step 的倍数。
    // 返回新指针（可能与 ptr 不同；扩展会丢弃旧内容）。失败返回 nullptr。
    auto grow_slot(void* ptr, size_t min_size, size_t step) -> void* {
        if (ptr == nullptr || min_size == 0) {
            return ptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto* slot = static_cast<uint8_t*>(ptr);
        auto it = ptr_sizes_.find(slot);
        if (it == ptr_sizes_.end()) {
            return nullptr;
        }
        if (it->second >= min_size) {
            return ptr;
        }

        size_t new_size = min_size;
        if (step > 0 && it->second + step >= it->second) {
            const auto base = it->second;
            const auto deficit = min_size - base;
            const auto steps = (deficit + step - 1) / step;
            new_size = base + steps * step;
        }

        all_ptrs_.erase(slot);
        ptr_sizes_.erase(it);
        std::free(slot);

        auto* new_ptr = alloc_slot(new_size);
        return new_ptr;
    }

    auto slot_size(void* ptr) const -> size_t {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = ptr_sizes_.find(static_cast<uint8_t*>(ptr));
        return it != ptr_sizes_.end() ? it->second : 0;
    }

    auto elem_size() const -> size_t {
        return elem_size_;
    }

private:
    auto alloc_slot(size_t size) -> uint8_t* {
        auto* ptr = static_cast<uint8_t*>(std::malloc(size));
        if (ptr == nullptr) {
            return nullptr;
        }
        std::memset(ptr, 0, size);
        all_ptrs_.insert(ptr);
        ptr_sizes_[ptr] = size;
        return ptr;
    }

    std::set<uint8_t*>         all_ptrs_;
    std::map<uint8_t*, size_t> ptr_sizes_;
    std::deque<uint8_t*>       free_list_;
    mutable std::mutex         mutex_;
    size_t                     elem_size_;
};

} // namespace scheduler
