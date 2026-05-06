#pragma once

#include <cstddef>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "DataHub.h"
#include "Task.h"
#include "Type.h"

namespace scheduler {

class TaskFlow {
public:
    TaskFlow(const std::string& name) : name_(name) {}
    virtual ~TaskFlow() = default;

    TaskFlow(const TaskFlow&) = delete;
    TaskFlow& operator=(const TaskFlow&) = delete;

    const std::string& name() const { return name_; }

    void add_task(std::unique_ptr<Task> task) {
        printf("[任务流] 任务流 '%s' 添加任务: '%s'\n",
               name_.c_str(), task->name().c_str());
        if (task == nullptr) {
            return;
        }
        tasks_.push_back(std::move(task));
    }

    template <typename T, typename Id>
    void register_data(Id id,
                       size_t capacity = DefaultBufferCapacity,
                       copy_func_t copy_func = nullptr) {
        const auto raw_id = static_cast<data_type_id_t>(id);
        data_meta_info_t info = {raw_id, capacity, sizeof(T), copy_func};
        data_registry_[raw_id] = [info](DataHub& hub) -> bool {
            return hub.add_data_buffer<T>(info);
        };
        printf("[任务流] 任务流 '%s' 注册数据: 数据类型 %u, 容量 %zu, %s拷贝函数\n",
               name_.c_str(), raw_id, capacity,
               copy_func == nullptr ? "无" : "有");
    }

    // 当前任务流中所有注册的数据，在DataHub中创建数据缓冲区
    bool init_data_hub(DataHub& hub) const {
        for (const auto& kv : data_registry_) {
            if (!kv.second(hub)) {
                return false;
            }
            printf("[任务流] 任务流 '%s' 初始化数据缓冲区: 数据类型 %u\n",
                   name_.c_str(), kv.first);
        }
        return true;
    }

    // 校验当前任务流中所有任务提及的数据元信息是否都被注册过
    bool validate_data_registry() const {
        std::set<data_type_id_t> all_used;
        for (const auto& t : tasks_) {
            if (!t) {
                continue;
            }
            for (auto id : t->consumes()) {
                all_used.insert(id);
            }
            for (auto id : t->produces()) {
                all_used.insert(id);
            }
        }
        for (auto id : all_used) {
            if (data_registry_.find(id) == data_registry_.end()) {
                printf("[任务流校验] 数据类型未注册: 任务流 = '%s', 数据类型 = %u\n",
                       name_.c_str(), id);
                return false;
            }
        }
        return true;
    }

    std::vector<data_type_id_t> all_consumes() const {
        if (!all_consumes_cached_) {
            std::set<data_type_id_t> unique;
            for (const auto& t : tasks_) {
                const auto& c = t->consumes();
                unique.insert(c.begin(), c.end());
            }
            all_consumes_ = std::vector<data_type_id_t>(unique.begin(), unique.end());
            all_consumes_cached_ = true;
        }
        return all_consumes_;
    }

    std::vector<data_type_id_t> all_produces() const {
        if (!all_produces_cached_) {
            std::set<data_type_id_t> unique;
            for (const auto& t : tasks_) {
                const auto& p = t->produces();
                unique.insert(p.begin(), p.end());
            }
            all_produces_ = std::vector<data_type_id_t>(unique.begin(), unique.end());
            all_produces_cached_ = true;
        }
        return all_produces_;
    }

    const std::vector<std::unique_ptr<Task>>& tasks() const noexcept {
        return tasks_;
    }

    auto data_registry() -> std::map<data_type_id_t, std::function<bool(DataHub&)>>& {
        return data_registry_;
    }

private:
    mutable std::vector<data_type_id_t> all_consumes_;
    mutable std::vector<data_type_id_t> all_produces_;
    mutable bool all_consumes_cached_ = false;
    mutable bool all_produces_cached_ = false;

    std::string name_;
    std::vector<std::unique_ptr<Task>> tasks_;
    std::map<data_type_id_t, std::function<bool(DataHub&)>> data_registry_;
};

} // namespace scheduler
