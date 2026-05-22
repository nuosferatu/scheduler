#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <shared_mutex>
#include <string>
#include <utility>

#include "scheduler/DataPool.h"
#include "scheduler/PerfMonitor.h"
#include "scheduler/Type.h"

namespace scheduler {

class DataBuffer {
public:
    /**
     * Write lease
     *   Use write_lease() to acquire a free memory slot
     *   Use get() to obtain the memory pointer (or as<T>() for typed access)
     *   Use commit() to commit
     *   Use abort() to discard; the destructor aborts automatically
     */
    class WriteLease {
    public:
        WriteLease() = default;

        // Failure path: only the status is filled, no real slot is held
        explicit WriteLease(data_access_status_t status)
            : status_(status) {}

        // Success path: OK status + real slot
        WriteLease(DataBuffer* owner, frame_id_t frame, void* ptr)
            : owner_(owner), frame_(frame), ptr_(ptr),
              status_(DataAccessStatus::OK) {}

        ~WriteLease() {
            abort();  // automatically abort on destruction
        }

        WriteLease(const WriteLease&)            = delete;
        WriteLease& operator=(const WriteLease&) = delete;

        WriteLease(WriteLease&& other) noexcept {
            move_from(std::move(other));
        }

        auto operator=(WriteLease&& other) noexcept -> WriteLease& {
            if (this != &other) {
                abort();
                move_from(std::move(other));
            }
            return *this;
        }

        // Get the memory pointer for writing data
        auto get() -> void* {
            return ptr_;
        }

        template <typename T>
        auto as() -> T* {
            return static_cast<T*>(ptr_);
        }

        // Request at least min_size bytes of capacity.
        //  If the current slot is too small, grow it by the data type's grow_step,
        //  but the old contents will be discarded.
        //  After growing, the internal pointer may change; re-fetch via as()/get().
        auto ensure_capacity(size_t min_size) -> bool {
            if (!ok()) {
                return false;
            }
            return owner_->grow_lease(ptr_, min_size);
        }

        // Commit the data and release the lease
        auto commit() -> bool {
            if (!valid()) {
                return false;
            }
            if (!owner_->commit_write(frame_, ptr_)) {
                return false;
            }
            owner_ = nullptr;
            ptr_ = nullptr;
            return true;
        }

        // Discard the data and release the lease; automatically aborted on destruction
        void abort() {
            if (owner_ != nullptr && ptr_ != nullptr) {
                owner_->abort_write(ptr_);
                owner_ = nullptr;
                ptr_ = nullptr;
            }
        }

        auto valid() const -> bool {
            return owner_ != nullptr && ptr_ != nullptr;
        }

        auto status() const -> data_access_status_t {
            return status_;
        }

        auto ok() const -> bool {
            return status_ == DataAccessStatus::OK && valid();
        }

    private:
        void move_from(WriteLease&& other) noexcept {
            owner_  = other.owner_;
            frame_  = other.frame_;
            ptr_    = other.ptr_;
            status_ = other.status_;
            other.owner_  = nullptr;
            other.ptr_    = nullptr;
            other.status_ = DataAccessStatus::INTERNAL_ERROR;
        }

        DataBuffer*          owner_  = nullptr;
        frame_id_t           frame_  = 0;
        void*                ptr_    = nullptr;
        data_access_status_t status_ = DataAccessStatus::INTERNAL_ERROR;
    };

    /**
     * Read lease
     *   Use read_lease() to acquire a memory slot
     *   Use get() to obtain the memory pointer (or as<T>() for typed access)
     *   Use release() to release; the destructor releases automatically
     */
    class ReadLease {
    public:
        ReadLease() = default;

        // Failure path: only the status is filled, no real slot is held
        explicit ReadLease(data_access_status_t status)
            : status_(status) {}

        // Success path: OK status + real slot
        ReadLease(DataBuffer* owner, frame_id_t frame, void* ptr)
            : owner_(owner), frame_(frame), ptr_(ptr),
              status_(DataAccessStatus::OK) {}

        ~ReadLease() {
            release();
        }

        ReadLease(const ReadLease&)            = delete;
        ReadLease& operator=(const ReadLease&) = delete;

        ReadLease(ReadLease&& other) noexcept {
            move_from(std::move(other));
        }

        auto operator=(ReadLease&& other) noexcept -> ReadLease& {
            if (this != &other) {
                release();
                move_from(std::move(other));
            }
            return *this;
        }

        // Get the memory pointer for reading data
        auto get() const -> const void* {
            return ptr_;
        }

        template <typename T>
        auto as() const -> const T* {
            return static_cast<const T*>(ptr_);
        }

        auto valid() const -> bool {
            return owner_ != nullptr && ptr_ != nullptr;
        }

        auto status() const -> data_access_status_t {
            return status_;
        }

        auto ok() const -> bool {
            return status_ == DataAccessStatus::OK && valid();
        }

    private:
        // Release the lease; automatically released on destruction
        void release() {
            if (owner_ != nullptr && ptr_ != nullptr) {
                owner_->unpin(ptr_, frame_);
                owner_ = nullptr;
                ptr_ = nullptr;
            }
        }

        void move_from(ReadLease&& other) noexcept {
            owner_  = other.owner_;
            frame_  = other.frame_;
            ptr_    = other.ptr_;
            status_ = other.status_;
            other.owner_  = nullptr;
            other.ptr_    = nullptr;
            other.status_ = DataAccessStatus::INTERNAL_ERROR;
        }

        DataBuffer*          owner_  = nullptr;
        frame_id_t           frame_  = 0;
        void*                ptr_    = nullptr;
        data_access_status_t status_ = DataAccessStatus::INTERNAL_ERROR;
    };

    DataBuffer(size_t              capacity,
               size_t              elem_size,
               copy_func_t         copy_func             = nullptr,
               release_slot_func_t release_slot_func     = nullptr,
               init_slot_func_t    init_slot_func        = nullptr,
               std::string         name                  = "",
               size_t              grow_step             = 0,
               size_t              write_wait_timeout_ms = 0)
        : pool_(capacity + 0, elem_size),
          capacity_(capacity),
          elem_size_(elem_size),
          grow_step_(grow_step),
          write_wait_timeout_ms_(write_wait_timeout_ms),
          copy_func_(std::move(copy_func)),
          release_slot_func_(std::move(release_slot_func)),
          init_slot_func_(std::move(init_slot_func)),
          name_(std::move(name)) {}

    ~DataBuffer() {
        if (!release_slot_func_) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto& kv : frame_vs_ptr_) {
            if (kv.second != nullptr) {
                release_slot_func_(kv.second);
            }
        }
    }

    DataBuffer(const DataBuffer&)            = delete;
    DataBuffer& operator=(const DataBuffer&) = delete;
    DataBuffer(DataBuffer&&)                 = delete;
    DataBuffer& operator=(DataBuffer&&)      = delete;

    auto size() -> size_t {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return buffer_.size();
    }

    auto capacity() const -> size_t {
        return capacity_;
    }

    auto elem_size() const -> size_t {
        return elem_size_;
    }

    auto name() const -> const std::string& {
        return name_;
    }

    // Read-only
    auto read_lease(frame_id_t frame) -> ReadLease {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = frame_vs_ptr_.find(frame);
        if (it == frame_vs_ptr_.end()) {
            return ReadLease(DataAccessStatus::FRAME_NOT_FOUND);
        }
        PerfMonitor::instance().on_buffer_first_read(name_, frame);
        const auto new_count = ++pin_counts_[it->second];
        printf("[DataBuffer] [Pin]   buffer='%s' frame=%u pin_count=%zu\n",
               name_.c_str(), frame, new_count);
        return ReadLease(this, frame, it->second);
    }

    void dump_pinned() {
        // std::shared_lock<std::shared_mutex> lock(mutex_);
        // if (pin_counts_.empty()) {
        //     printf("[DataBuffer] [Dump]  buffer='%s' no pinned slots\n", name_.c_str());
        //     return;
        // }
        // printf("[DataBuffer] [Dump]  buffer='%s' pinned slots: %zu\n",
        //        name_.c_str(), pin_counts_.size());
        // for (const auto& [ptr, count] : pin_counts_) {
        //     frame_id_t frame = 0;
        //     bool       found = false;
        //     for (const auto& [f, p] : frame_vs_ptr_) {
        //         if (p == ptr) {
        //             frame = f;
        //             found = true;
        //             break;
        //         }
        //     }
        //     if (found) {
        //         printf("[DataBuffer] [Dump]    buffer='%s' frame=%u pin_count=%zu\n",
        //                name_.c_str(), frame, count);
        //     } else {
        //         printf("[DataBuffer] [Dump]    buffer='%s' frame=<unknown> ptr=%p pin_count=%zu\n",
        //                name_.c_str(), ptr, count);
        //     }
        // }
    }

    // Copy data into a memory slot
    auto write(frame_id_t frame, const void* data) -> bool {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        // TODO: decide whether to overwrite on repeated writes
        size_t size_snap = buffer_.size();

        auto existing = frame_vs_ptr_.find(frame);
        if (existing != frame_vs_ptr_.end()) {
            if (is_pinned_locked(existing->second)) {
                return false;
            }
            erase_frame_locked(frame);
            release_slot_locked(existing->second);
            frame_vs_ptr_.erase(existing);
            PerfMonitor::instance().on_buffer_cancel_insert_wait(name_, frame);
            PerfMonitor::instance().on_buffer_size_snap(
                name_, capacity_, size_snap, buffer_.size(), "overwrite_erase");
        }

        if (buffer_.size() >= capacity_ && !pop_unpinned_locked()) {
            return false;
        }
        PerfMonitor::instance().on_buffer_size_snap(
            name_, capacity_, size_snap, buffer_.size(), "evict");

        auto ptr = pool_.acquire_ptr();
        if (ptr == nullptr) {
            printf("[DataBuffer] [Error] data pool is full\n");
            return false;
        }

        if (init_slot_func_) {
            init_slot_func_(ptr, pool_.slot_size(ptr));
        }

        if (copy_func_) {
            copy_func_(ptr, data);
        } else {
            std::memcpy(ptr, data, pool_.slot_size(ptr));
        }

        buffer_.push_back(frame);
        frame_vs_ptr_[frame] = ptr;
        PerfMonitor::instance().on_buffer_insert(name_, frame);
        PerfMonitor::instance().on_buffer_size_snap(
            name_, capacity_, size_snap, buffer_.size(), "insert");
        return true;
    }

    // Acquire a write lease and write data directly into the memory slot
    auto write_lease(frame_id_t frame) -> WriteLease {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        auto ptr = pool_.acquire_ptr();
        if (ptr == nullptr) {
            size_t size_snap = buffer_.size();
            bool got_slot = pop_unpinned_locked();

            if (!got_slot && write_wait_timeout_ms_ > 0) {
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(write_wait_timeout_ms_);
                got_slot = slot_available_cv_.wait_until(
                    lock, deadline, [this] { return pop_unpinned_locked(); });
            }

            if (!got_slot) {
                if (write_wait_timeout_ms_ == 0) {
                    printf("[DataBuffer] [Warn] '%s' write_lease drop frame %u: "
                           "all slots pinned (previous frame still in flight)\n",
                           name_.c_str(), frame);
                } else {
                    printf("[DataBuffer] [Warn] '%s' write_lease drop frame %u: "
                           "wait %zums expired, all slots still pinned\n",
                           name_.c_str(), frame, write_wait_timeout_ms_);
                }
                return WriteLease(DataAccessStatus::POOL_FULL);
            }

            PerfMonitor::instance().on_buffer_size_snap(
                name_, capacity_, size_snap, buffer_.size(), "lease_evict");
            ptr = pool_.acquire_ptr();
            if (ptr == nullptr) {
                printf("[DataBuffer] [Error] '%s' write_lease acquire failed after evict, "
                       "frame %u\n", name_.c_str(), frame);
                return WriteLease(DataAccessStatus::POOL_FULL);
            }
        }

        if (init_slot_func_) {
            init_slot_func_(ptr, pool_.slot_size(ptr));
        }

        return WriteLease(this, frame, ptr);
    }

private:
    // Grow the slot pointed to by ptr to at least min_size bytes, rounded up by grow_step_.
    //   ptr: in/out parameter; on successful growth with reallocation, it is updated to the new pointer.
    auto grow_lease(void*& ptr, size_t min_size) -> bool {
        if (ptr == nullptr) {
            return false;
        }
        if (min_size == 0 || pool_.slot_size(ptr) >= min_size) {
            return true;
        }
        auto* new_ptr = pool_.grow_slot(ptr, min_size, grow_step_);
        if (new_ptr == nullptr) {
            return false;
        }
        if (new_ptr != ptr) {
            if (init_slot_func_) {
                init_slot_func_(new_ptr, pool_.slot_size(new_ptr));
            }
            ptr = new_ptr;
        }
        return true;
    }

    auto commit_write(frame_id_t frame, void* ptr) -> bool {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        size_t size_snap = buffer_.size();

        auto existing = frame_vs_ptr_.find(frame);
        if (existing != frame_vs_ptr_.end()) {
            if (is_pinned_locked(existing->second)) {
                return false;
            }
            erase_frame_locked(frame);
            release_slot_locked(existing->second);
            frame_vs_ptr_.erase(existing);
            PerfMonitor::instance().on_buffer_cancel_insert_wait(name_, frame);
            PerfMonitor::instance().on_buffer_size_snap(
                name_, capacity_, size_snap, buffer_.size(), "overwrite_erase");
        }

        if (buffer_.size() >= capacity_ && !pop_unpinned_locked()) {
            return false;
        }
        PerfMonitor::instance().on_buffer_size_snap(
            name_, capacity_, size_snap, buffer_.size(), "evict");

        buffer_.push_back(frame);
        frame_vs_ptr_[frame] = ptr;
        PerfMonitor::instance().on_buffer_insert(name_, frame);
        PerfMonitor::instance().on_buffer_size_snap(
            name_, capacity_, size_snap, buffer_.size(), "insert");
        return true;
    }

    void abort_write(void* ptr) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        release_slot_locked(ptr);
    }

    void unpin(void* ptr, frame_id_t frame) {
        bool became_unpinned = false;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto it = pin_counts_.find(ptr);
            if (it == pin_counts_.end()) {
                printf("[DataBuffer] [Unpin] [Warn] buffer='%s' frame=%u ptr=%p not pinned\n",
                       name_.c_str(), frame, ptr);
                return;
            }
            if (it->second <= 1) {
                pin_counts_.erase(it);
                became_unpinned = true;
                printf("[DataBuffer] [Unpin] buffer='%s' frame=%u pin_count=0\n",
                       name_.c_str(), frame);
            } else {
                --it->second;
                printf("[DataBuffer] [Unpin] buffer='%s' frame=%u pin_count=%zu\n",
                       name_.c_str(), frame, it->second);
            }
        }
        // After pin drops to 0, a waiting write_lease may now be able to evict this slot
        if (became_unpinned) {
            slot_available_cv_.notify_all();
        }
    }

    auto is_pinned_locked(void* ptr) const -> bool {
        auto it = pin_counts_.find(ptr);
        return it != pin_counts_.end() && it->second > 0;
    }

    void erase_frame_locked(frame_id_t frame) {
        for (auto it = buffer_.begin(); it != buffer_.end(); ++it) {
            if (*it == frame) {
                buffer_.erase(it);
                return;
            }
        }
    }

    void release_slot_locked(void* ptr) {
        if (release_slot_func_) {
            release_slot_func_(ptr);
        }
        pool_.release_ptr(ptr);
        slot_available_cv_.notify_all();
    }

    auto pop_unpinned_locked() -> bool {
        for (auto bit = buffer_.begin(); bit != buffer_.end(); ++bit) {
            auto fit = frame_vs_ptr_.find(*bit);
            if (fit == frame_vs_ptr_.end()) {
                buffer_.erase(bit);
                return true;
            }
            if (is_pinned_locked(fit->second)) {
                continue;
            }
            const frame_id_t frame = *bit;
            void* const ptr = fit->second;
            buffer_.erase(bit);
            frame_vs_ptr_.erase(fit);
            release_slot_locked(ptr);
            PerfMonitor::instance().on_buffer_evicted_before_read(name_, frame);
            return true;
        }
        return false;
    }

    DataPool                      pool_;
    std::deque<frame_id_t>        buffer_;                 // logic buffer, FIFO
    std::map<frame_id_t, void*>   frame_vs_ptr_;           // frame -> slot, O(1) lookup
    std::map<void*, size_t>       pin_counts_;             // for using data protection
    std::shared_mutex             mutex_;
    std::condition_variable_any   slot_available_cv_;      // used while write_lease waits for a slot
    size_t                        capacity_;
    size_t                        elem_size_;
    size_t                        grow_step_;              // slot grow step (0 means allocate exactly to min_size)
    size_t                        write_wait_timeout_ms_;  // write_lease wait timeout for a slot (0 means do not wait)

    copy_func_t         copy_func_;
    release_slot_func_t release_slot_func_;
    init_slot_func_t    init_slot_func_;
    std::string         name_;
};

} // namespace scheduler
