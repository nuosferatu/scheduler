#pragma once

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "scheduler/DataType.h"
#include "scheduler/Task.h"
#include "scheduler/Type.h"

namespace scheduler {

class TaskFlow {
public:
    TaskFlow(const std::string& name) : name_(name) {}
    virtual ~TaskFlow() = default;

    TaskFlow(const TaskFlow&)            = delete;
    TaskFlow& operator=(const TaskFlow&) = delete;

    auto name() const -> const std::string& { return name_; }

    virtual auto init() -> bool { return true; }
    virtual void stop() {}

    virtual auto data_type_descriptors() const
        -> std::vector<std::shared_ptr<const DataTypeDescriptor>> {
        return data_type_descriptors_;
    }

    auto tasks() const -> const std::vector<std::unique_ptr<Task>>& {
        return tasks_;
    }

    auto data_type_name(data_type_id_t id) const -> const char* {
        const auto it = data_names_.find(id);
        if (it == data_names_.end()) {
            return "<unknown>";
        }
        return it->second;
    }

    void add_data_type(std::shared_ptr<const DataTypeDescriptor> descriptor) {
        if (descriptor == nullptr) {
            printf("[TaskFlow] [Warn] '%s' add_data_type: null descriptor, skip\n",
                   name_.c_str());
            return;
        }
        const auto id = descriptor->id();
        if (data_names_.find(id) != data_names_.end()) {
            printf("[TaskFlow] [Warn] '%s' data type %u:%s already added, skip\n",
                   name_.c_str(), id, descriptor->name());
            return;
        }
        printf("[TaskFlow] '%s' added data type %u:%s\n",
               name_.c_str(), id, descriptor->name());
        data_names_.emplace(id, descriptor->name());
        data_type_descriptors_.push_back(std::move(descriptor));
    }

    void add_data_type(data_type_id_t      id,
                       std::string         name,
                       size_t              elem_size,
                       size_t              capacity          = DefaultBufferCapacity,
                       copy_func_t         copy_func         = nullptr,
                       release_slot_func_t release_slot_func = nullptr,
                       init_slot_func_t    init_slot_func    = nullptr) {
        add_data_type(std::make_shared<DefaultDataTypeDescriptor>(
            id, std::move(name), elem_size, capacity,
            std::move(copy_func), std::move(release_slot_func), std::move(init_slot_func)));
    }

    void add_task(std::unique_ptr<Task> task) {
        if (task == nullptr) {
            return;
        }
        task->owner_ = this;

        printf("[TaskFlow] '%s' added task '%s', consumes: %s, produces: %s\n",
            name().c_str(), task->name().c_str(),
            format_ids(task->consumes()).c_str(),
            format_ids(task->produces()).c_str());

        tasks_.push_back(std::move(task));
    }

    auto validate_data_types() -> bool {
        const auto descriptors = data_type_descriptors();
        std::set<data_type_id_t> ids;
        for (const auto& d : descriptors) {
            if (d != nullptr) {
                ids.insert(d->id());
            }
        }

        for (const auto& task : tasks_) {
            if (!task) {
                continue;
            }
            for (const auto& id : task->consumes()) {
                if (ids.count(id) == 0) {
                    printf("[TaskFlow] [Error] Data type %u not registered in task flow '%s'\n",
                           id, name().c_str());
                    return false;
                }
            }
            for (const auto& id : task->produces()) {
                if (ids.count(id) == 0) {
                    printf("[TaskFlow] [Error] Data type %u not registered in task flow '%s'\n",
                           id, name().c_str());
                    return false;
                }
            }
        }

        return true;
    }

private:
    auto format_ids(const std::vector<data_type_id_t>& ids) const -> std::string {
        std::string s;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i > 0) {
                s += ", ";
            }
            s += std::to_string(ids[i]) + ":" + data_type_name(ids[i]);
        }
        return s;
    }

    std::string                                            name_;
    std::vector<std::unique_ptr<Task>>                     tasks_;
    std::vector<std::shared_ptr<const DataTypeDescriptor>> data_type_descriptors_;
    std::unordered_map<data_type_id_t, const char*>        data_names_;
};

} // namespace scheduler
