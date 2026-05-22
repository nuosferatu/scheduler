#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "scheduler/DataHub.h"
#include "scheduler/Type.h"

namespace scheduler {

class DataTypeDescriptor {
public:
    virtual ~DataTypeDescriptor() = default;

    virtual auto id() const -> data_type_id_t = 0;
    virtual auto name() const -> const char* = 0;
    virtual auto elem_size() const -> size_t = 0;

    virtual auto capacity() const -> size_t {
        return DefaultBufferCapacity;
    }

    // Slot grow step: return 0 to keep the slot size fixed; when >0, DataPool grows
    // the slot rounded up by this step inside WriteLease::ensure_capacity().
    virtual auto grow_step() const -> size_t {
        return 0;
    }

    // Write wait policy
    virtual auto write_wait_timeout_ms() const -> size_t {
        return 0;
    }

    virtual auto init_slot(void* memory) const -> void* = 0;
    virtual void copy_slot(void* dst, const void* src) const = 0;

    virtual auto copy_func() const -> copy_func_t {
        return [this](void* dst, const void* src) {
            copy_slot(dst, src);
        };
    }

    virtual auto release_slot_func() const -> release_slot_func_t {
        return nullptr;
    }

    virtual auto init_slot_func() const -> init_slot_func_t {
        return [this](void* memory, size_t memory_size) {
            (void)memory_size;
            init_slot(memory);
        };
    }

    virtual auto install_into(DataHub& hub) const -> bool {
        return hub.create_buffer(
            id(), elem_size(), capacity(),
            copy_func(), release_slot_func(), init_slot_func(),
            grow_step(), write_wait_timeout_ms());
    }

    virtual auto init_storage(std::vector<uint8_t>& storage, void*& data) const -> bool {
        const auto size = elem_size();
        if (size == 0) {
            return false;
        }

        storage.resize(size);
        std::memset(storage.data(), 0, storage.size());
        data = init_slot(storage.data());
        return data != nullptr;
    }
};

class DefaultDataTypeDescriptor : public DataTypeDescriptor {
public:
    DefaultDataTypeDescriptor(data_type_id_t      id,
                              std::string         name,
                              size_t              elem_size,
                              size_t              capacity          = DefaultBufferCapacity,
                              copy_func_t         copy_func         = nullptr,
                              release_slot_func_t release_slot_func = nullptr,
                              init_slot_func_t    init_slot_func    = nullptr)
        : id_(id),
          name_(std::move(name)),
          elem_size_(elem_size),
          capacity_(capacity == 0 ? DefaultBufferCapacity : capacity),
          copy_func_(std::move(copy_func)),
          release_slot_func_(std::move(release_slot_func)),
          init_slot_func_(std::move(init_slot_func)) {}

    auto id() const -> data_type_id_t override { return id_; }
    auto name() const -> const char* override { return name_.c_str(); }
    auto elem_size() const -> size_t override { return elem_size_; }
    auto capacity() const -> size_t override { return capacity_; }

    auto init_slot(void* memory) const -> void* override { return memory; }

    void copy_slot(void* dst, const void* src) const override {
        std::memcpy(dst, src, elem_size_);
    }

    auto copy_func() const -> copy_func_t override {
        return copy_func_ ? copy_func_ : DataTypeDescriptor::copy_func();
    }

    auto release_slot_func() const -> release_slot_func_t override {
        return release_slot_func_;
    }

    auto init_slot_func() const -> init_slot_func_t override {
        return init_slot_func_ ? init_slot_func_ : DataTypeDescriptor::init_slot_func();
    }

private:
    data_type_id_t      id_;
    std::string         name_;
    size_t              elem_size_;
    size_t              capacity_;
    copy_func_t         copy_func_;
    release_slot_func_t release_slot_func_;
    init_slot_func_t    init_slot_func_;
};

} // namespace scheduler
