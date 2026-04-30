#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <opencv2/opencv.hpp>

#include "DataHub.h"
#include "TaskContext.h"
#include "Type.h"

namespace scheduler {

class Task {
public:
    Task(const std::string& name) : name_(name) {}
    virtual ~Task() = default;

    // 禁止拷贝/赋值：多态基类，拷贝会产生切片与错误所有权语义。
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    const std::string& name() const { return name_; }
    virtual uint8_t priority() const { return 0; }

    virtual const std::vector<DataTypeId>& consumes() const = 0;
    virtual const std::vector<DataTypeId>& produces() const = 0;

    virtual bool init(TaskContext& ctx) { return true; }  // 只会调用一次
    virtual bool run(frame_id_t frame_id, TaskContext& ctx) = 0;
    virtual void stop(TaskContext& ctx) {}  // 只会调用一次

private:
    std::string name_;
};

class TaskFlowTemplate {
public:
    TaskFlowTemplate(const std::string& name) : name_(name) {}
    virtual ~TaskFlowTemplate() = default;

    // 禁止拷贝/赋值：持有 unique_ptr 任务列表与注册闭包，不可复制。
    TaskFlowTemplate(const TaskFlowTemplate&) = delete;
    TaskFlowTemplate& operator=(const TaskFlowTemplate&) = delete;

    const std::string& name() const { return name_; }

    void add_task(std::unique_ptr<Task> task) {
        printf("[任务流] 任务流 '%s' 添加任务: '%s'\n",
               name_.c_str(), task->name().c_str());
        if (task == nullptr) {
            return;
        }
        tasks_.push_back(std::move(task));
    }

    // 任务流中的任务使用的所有数据类型，都要将元信息注册到任务流中
    //  在调度器启动前，会注册到DataHub中，缺失元信息会导致校验失败，无法启动
    template <typename T>
    void register_data(DataTypeId id,
                       size_t capacity = DefaultBufferCapacity,
                       copy_func_t copy_func = nullptr) {
        data_meta_info_t info = {id, capacity, sizeof(T), copy_func};
        data_registry_[id] = [info](DataHub& hub) -> bool {
            return hub.add_data_buffer<T>(info);
        };
        printf("[任务流] 任务流 '%s' 注册数据: 数据类型 %u, 容量 %zu, %s拷贝函数\n",
               name_.c_str(), static_cast<uint32_t>(id), capacity,
               copy_func == nullptr ? "无" : "有");
    }

    // 当前任务流中所有注册的数据，在DataHub中创建数据缓冲区
    bool init_data_hub(DataHub& hub) const {
        for (const auto& kv : data_registry_) {
            if (!kv.second(hub)) {
                return false;
            }
            printf("[任务流] 任务流 '%s' 初始化数据缓冲区: 数据类型 %u\n", name_.c_str(), static_cast<uint32_t>(kv.first));
        }
        return true;
    }

    // 校验当前任务流中所有任务提及的数据元信息是否都被注册过
    bool validate_data_registry() const {
        std::set<DataTypeId> all_used;
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
                       name_.c_str(), static_cast<uint32_t>(id));
                return false;
            }
        }
        return true;
    }

    const std::vector<std::unique_ptr<Task>>& tasks() const noexcept {
        return tasks_;
    }

    Task* find(const std::string& task_name) const {
        for (const auto& t : tasks_) {
            if (t && t->name() == task_name) {
                return t.get();
            }
        }
        return nullptr;
    }

    std::vector<DataTypeId> all_consumes() const {
        if (!all_consumes_cached_) {
            std::set<DataTypeId> unique;
            for (const auto& t : tasks_) {
                const auto& c = t->consumes();
                unique.insert(c.begin(), c.end());
            }
            all_consumes_ = std::vector<DataTypeId>(unique.begin(), unique.end());
            all_consumes_cached_ = true;
        }
        return all_consumes_;
    }

    std::vector<DataTypeId> all_produces() const {
        if (!all_produces_cached_) {
            std::set<DataTypeId> unique;
            for (const auto& t : tasks_) {
                const auto& p = t->produces();
                unique.insert(p.begin(), p.end());
            }
            all_produces_ = std::vector<DataTypeId>(unique.begin(), unique.end());
            all_produces_cached_ = true;
        }
        return all_produces_;
    }

private:
    mutable std::vector<DataTypeId> all_consumes_;
    mutable std::vector<DataTypeId> all_produces_;
    mutable bool all_consumes_cached_ = false;
    mutable bool all_produces_cached_ = false;

    std::string name_;
    std::vector<std::unique_ptr<Task>> tasks_;
    std::map<DataTypeId, std::function<bool(DataHub&)>> data_registry_;
};

} // namespace scheduler
