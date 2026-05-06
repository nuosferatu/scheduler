#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "DataHub.h"
#include "Task.h"
#include "TaskContext.h"
#include "TaskFlow.h"
#include "Type.h"
#include "Worker.h"

namespace scheduler {

class Scheduler {
public:
    Scheduler() {
        printf("[调度器] 创建调度器\n");
    }
    ~Scheduler() { stop(); }

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    void register_task_flow(std::unique_ptr<TaskFlow> flow) {
        if (flow == nullptr) {
            return;
        }
        printf("[调度器] 注册任务流 '%s'\n", flow->name().c_str());
        task_flows_.push_back(std::move(flow));
    }

    bool init_data_hub() {
        printf("[调度器] >>> 开始校验数据元信息\n");
        for (auto& flow : task_flows_) {
            if (!flow->validate_data_registry()) {
                printf("[调度器] <<< 数据元信息校验失败\n");
                return false;
            }
        }
        printf("[调度器] <<< 数据元信息校验通过\n");

        printf("[调度器] >>> 开始校验数据类型ID唯一性\n");
        if (!validate_data_id_uniqueness()) {
            printf("[调度器] <<< 数据类型ID唯一性校验失败\n");
            return false;
        }
        printf("[调度器] <<< 数据类型ID唯一性校验通过\n");

        printf("[调度器] >>> 开始初始化数据缓冲区\n");
        for (auto& flow : task_flows_) {
            if (!flow->init_data_hub(data_hub_)) {
                printf("[调度器] <<< 数据缓冲区初始化失败\n");
                return false;
            }
        }
        printf("[调度器] <<< 数据缓冲区初始化完成\n");

        return true;
    }

    // 检查不同任务流之间是否有重复注册同一 data_type_id 的情况。
    bool validate_data_id_uniqueness() const {
        std::map<data_type_id_t, std::string> id_owner;
        for (const auto& flow : task_flows_) {
            if (!flow) {
                continue;
            }
            for (auto& [id, _] : flow->data_registry()) {
                auto it = id_owner.find(id);
                if (it != id_owner.end()) {
                    printf("[调度器] [Error] 数据类型 %u 被多个任务流重复注册 "
                           "(任务流 '%s' 和 '%s')\n",
                           id, it->second.c_str(), flow->name().c_str());
                    return false;
                }
                id_owner.emplace(id, flow->name());
            }
        }
        return true;
    }

    bool start() {
        if (running_.load()) {
            return false;
        }
        if (!build_static_info()) {
            return false;
        }
        if (!init_data_hub()) {
            return false;
        }
        init_workers();
        start_workers();
        running_.store(true);
        return true;
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        stop_workers();
        clear_in_flight();
    }

    // 外部数据输入
    template <typename T, typename Id>
    bool feed(frame_id_t frame_id, Id id, const T& data) {
        const auto raw_id = static_cast<data_type_id_t>(id);
        if (!running_.load()) {
            printf("[调度器] 调度器未运行\n");
            return false;
        }
        auto producer_it = producer_of_.find(raw_id);
        if (producer_it != producer_of_.end()) {
            printf("[调度器] 数据类型 %u 是内部数据 "
                   "(由 '%s' 生产), 不是外部输入\n",
                   raw_id,
                   producer_it->second.c_str());
            return false;
        }
        if (external_inputs_.count(raw_id) == 0) {
            printf("[调度器] 数据类型 %u 不是已注册的外部输入\n", raw_id);
            return false;
        }
        if (!data_hub_.push_data<T>(raw_id, frame_id, data)) {
            return false;
        }
        on_data_arrived(frame_id, raw_id);
        return true;
    }

private:
    // 帧实例，保存一帧中所有任务的状态信息
    struct FrameInstance {
        std::map<std::string, TaskStatus> task_status;   // 任务状态
        std::set<data_type_id_t>          arrived;       // 已到达的数据类型
        std::set<data_type_id_t>          failed;        // 已失效的数据类型（再也来不了了）
        size_t                            finished = 0;  // 完成的任务个数（包括 COMPLETED、FAILED、SKIPPED）
    };

    // 待分发的任务
    struct ReadyItem {
        frame_id_t  frame_id = 0;
        std::string task_name;
    };

    bool build_static_info() {
        tasks_.clear();
        producer_of_.clear();
        external_inputs_.clear();

        printf("[调度器] >>> 开始构建任务信息\n");
        if (!build_tasks()) {
            printf("[调度器] <<< 任务信息构建失败\n");
            return false;
        }
        printf("[调度器] <<< 任务信息构建完成\n");

        printf("[调度器] >>> 开始构建生产者信息\n");
        if (!build_producer_of()) {
            printf("[调度器] <<< 生产者信息构建失败\n");
            return false;
        }
        printf("[调度器] <<< 生产者信息构建完成\n");

        printf("[调度器] >>> 开始构建外部输入信息\n");
        if (!build_external_inputs()) {
            printf("[调度器] <<< 外部输入信息构建失败\n");
            return false;
        }
        printf("[调度器] <<< 外部输入信息构建完成\n");

        return true;
    }

    bool build_tasks() {
        for (auto& flow : task_flows_) {
            for (const auto& task : flow->tasks()) {
                if (!task) {
                    continue;
                }
                const std::string& name = task->name();
                auto it = tasks_.find(name);
                if (it != tasks_.end()) {
                    printf("[调度器] [Error] 任务已存在: '%s'\n", name.c_str());
                    return false;
                }
                tasks_.emplace(name, task.get());
                printf("[调度器] 发现任务 '%s'\n", name.c_str());
            }
        }
        return true;
    }

    bool build_producer_of() {
        for (auto& kv : tasks_) {
            const std::string& name = kv.first;
            const Task*        task = kv.second;
            for (auto id : task->produces()) {
                auto it = producer_of_.find(id);
                if (it != producer_of_.end()) {
                    printf("[调度器] [Error] 数据类型 %u 发现多个生产者 (任务 '%s' 和 '%s')\n",
                           id, it->second.c_str(), name.c_str());
                    return false;
                }
                producer_of_[id] = name;
                printf("[调度器] 数据类型 %u 发现生产者 '%s'\n", id, name.c_str());
            }
        }
        return true;
    }

    bool build_external_inputs() {
        std::set<data_type_id_t> all_produced;
        std::set<data_type_id_t> all_consumed;

        for (auto& kv : tasks_) {
            const Task* task = kv.second;
            for (auto id : task->consumes()) {
                all_consumed.insert(id);
            }
            for (auto id : task->produces()) {
                all_produced.insert(id);
            }
        }

        for (auto id : all_consumed) {
            if (all_produced.find(id) == all_produced.end()) {
                external_inputs_.insert(id);
                printf("[调度器] 发现外部输入数据类型 %u\n", id);
            }
        }

        return true;
    }

    void init_workers() {
        workers_.clear();
        for (auto& kv : tasks_) {
            const std::string& name = kv.first;
            Task*              task = kv.second;
            workers_[name] = std::make_unique<Worker>(
                name,
                task,
                std::make_unique<TaskContext>(
                    data_hub_,
                    task->priority() > 0,
                    [this]() {
                        on_release_priority_lock();
                    }
                ),
                [this](const std::string& name, frame_id_t fid, TaskStatus st) {
                    on_task_done(name, fid, st);
                }
            );
        }
    }

    void start_workers() {
        for (auto& [name, worker] : workers_) {
            worker->start();
        }
    }

    void stop_workers() {
        for (auto& [name, worker] : workers_) {
            worker->stop();
        }
    }

    void clear_in_flight() {
        std::lock_guard<std::mutex> lock(in_flight_mutex_);
        in_flight_.clear();
        ready_queue_.clear();
        priority_lock_held_ = false;
    }

    // 查找帧实例，如果不存在则创建
    FrameInstance& ensure_frame_locked(frame_id_t frame_id) {
        auto it = in_flight_.find(frame_id);
        if (it != in_flight_.end()) {
            return it->second;
        }

        auto emp = in_flight_.emplace(frame_id, FrameInstance{});
        FrameInstance& fi = emp.first->second;
        for (const auto& kv : tasks_) {
            fi.task_status.emplace(kv.first, TaskStatus::PENDING);
        }
        return fi;
    }

    void skip_failed_locked(FrameInstance& fi, frame_id_t frame_id) {
        while (mark_new_skipped_locked(fi)) {}
    }

    bool mark_new_skipped_locked(FrameInstance& fi) {
        bool skipped = false;
        for (auto& kv : fi.task_status) {
            if (kv.second != TaskStatus::PENDING) {
                continue;
            }
            Task* task = tasks_.at(kv.first);
            if (!has_failed_dependency(fi, task)) {
                continue;
            }

            kv.second = TaskStatus::SKIPPED;
            ++fi.finished;
            for (auto id : task->produces()) {
                fi.failed.insert(id);
            }
            skipped = true;
        }
        return skipped;
    }

    void enqueue_ready_locked(FrameInstance& fi, frame_id_t frame_id) {
        for (auto& kv : fi.task_status) {
            const std::string& name = kv.first;
            if (kv.second != TaskStatus::PENDING) {
                continue;
            }
            Task* task = tasks_.at(name);
            if (!has_all_inputs_arrived(fi, task)) {
                continue;
            }
            kv.second = TaskStatus::READY;
            ReadyItem item;
            item.frame_id = frame_id;
            item.task_name = name;
            ready_queue_.emplace(task->priority(), std::move(item));
        }
    }

    bool has_failed_dependency(const FrameInstance& fi, const Task* task) const {
        for (auto id : task->consumes()) {
            if (fi.failed.count(id) > 0) {
                return true;
            }
        }
        return false;
    }

    bool has_all_inputs_arrived(const FrameInstance& fi, const Task* task) const {
        for (auto id : task->consumes()) {
            if (fi.arrived.count(id) == 0) {
                return false;
            }
        }
        return true;
    }

    void on_data_arrived(frame_id_t frame_id, data_type_id_t id) {
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            FrameInstance& fi = ensure_frame_locked(frame_id);
            fi.arrived.insert(id);
            skip_failed_locked(fi, frame_id);
            enqueue_ready_locked(fi, frame_id);
            erase_frame_if_done_locked(frame_id, fi);
        }
        dispatch_pending();
    }

    void on_task_done(const std::string& name,
                        frame_id_t frame_id,
                        TaskStatus status) {
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            auto fit = in_flight_.find(frame_id);
            if (fit != in_flight_.end()) {
                FrameInstance& fi = fit->second;
                mark_task_finished_locked(fi, name, status);
                skip_failed_locked(fi, frame_id);
                enqueue_ready_locked(fi, frame_id);
                erase_frame_if_done_locked(frame_id, fi);
            }
        }
        dispatch_pending();
    }

    void mark_task_finished_locked(FrameInstance& fi,
                                    const std::string& name,
                                    TaskStatus status) {
        auto sit = fi.task_status.find(name);
        if (sit == fi.task_status.end()) {
            return;
        }
        sit->second = status;
        ++fi.finished;

        Task* task = tasks_.at(name);
        if (status == TaskStatus::COMPLETED) {
            for (auto id : task->produces()) {
                fi.arrived.insert(id);
            }
        } else {
            for (auto id : task->produces()) {
                fi.failed.insert(id);
            }
        }
    }

    void erase_frame_if_done_locked(frame_id_t frame_id, FrameInstance& fi) {
        if (fi.finished >= tasks_.size()) {
            in_flight_.erase(frame_id);
        }
    }

    // 调度决策持 in_flight_mutex_ 锁
    // enqueue 在锁外调用
    void dispatch_pending() {
        std::vector<ReadyItem> to_dispatch;
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            collect_dispatchable_locked(to_dispatch);
        }
        for (auto& item : to_dispatch) {
            auto wit = workers_.find(item.task_name);
            if (wit != workers_.end()) {
                wit->second->enqueue(item.frame_id);
            }
        }
    }

    void collect_dispatchable_locked(std::vector<ReadyItem>& out) {
        auto it = ready_queue_.begin();
        while (it != ready_queue_.end() && it->first == 0) {
            mark_running_locked(it->second);
            out.push_back(std::move(it->second));
            it = ready_queue_.erase(it);
        }
        if (!priority_lock_held_ && it != ready_queue_.end()) {
            priority_lock_held_ = true;
            mark_running_locked(it->second);
            out.push_back(std::move(it->second));
            ready_queue_.erase(it);
        }
    }

    void mark_running_locked(const ReadyItem& item) {
        auto fit = in_flight_.find(item.frame_id);
        if (fit == in_flight_.end()) {
            return;
        }
        auto sit = fit->second.task_status.find(item.task_name);
        if (sit != fit->second.task_status.end()) {
            sit->second = TaskStatus::RUNNING;
        }
    }

    // 高优先级任务结束时调用
    // 释放锁，并触发任务分发
    void on_release_priority_lock() {
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            priority_lock_held_ = false;
        }
        dispatch_pending();
    }

private:
    DataHub                                        data_hub_;
    std::vector<std::unique_ptr<TaskFlow>> task_flows_;
    std::map<std::string, std::unique_ptr<Worker>> workers_;

    // 静态信息
    std::map<std::string, Task*>          tasks_;            // 任务指针
    std::map<data_type_id_t, std::string> producer_of_;      // 数据的生产者
    std::set<data_type_id_t>              external_inputs_;  // 外部输入

    // 执行队列
    std::mutex                                       in_flight_mutex_;
    std::map<frame_id_t, FrameInstance>              in_flight_;
    std::multimap<uint8_t, ReadyItem>                ready_queue_;
    bool                                             priority_lock_held_ = false;

    std::atomic<bool>                                running_{false};
};

} // namespace scheduler
