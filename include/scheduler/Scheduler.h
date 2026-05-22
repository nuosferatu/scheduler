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

#include "scheduler/DataHub.h"
#include "scheduler/DataType.h"
#include "scheduler/Task.h"
#include "scheduler/TaskContext.h"
#include "scheduler/TaskFlow.h"
#include "scheduler/Type.h"
#include "scheduler/Worker.h"
#include "scheduler/PerfMonitor.h"

namespace scheduler {

class Scheduler {
public:
    Scheduler() {}
    ~Scheduler() { stop(); }

    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&)                 = delete;
    Scheduler& operator=(Scheduler&&)      = delete;

    void register_task_flow(std::unique_ptr<TaskFlow> flow) {
        if (flow == nullptr) {
            return;
        }
        for (const auto& descriptor : flow->data_type_descriptors()) {
            register_data_type(descriptor, flow->name());
        }
        printf("[INFO] [Scheduler] Task flow '%s' registered\n", flow->name().c_str());
        task_flows_.push_back(std::move(flow));
    }

    void register_data_type(std::shared_ptr<const DataTypeDescriptor> descriptor,
                            const std::string& source = {}) {
        if (descriptor == nullptr) {
            return;
        }
        const auto id = descriptor->id();
        auto it = data_type_descriptors_.find(id);
        if (it != data_type_descriptors_.end()) {
            printf("[Scheduler] [Warn] Data type %u:%s already registered, skip %s from '%s'\n",
                   id, it->second->name(), descriptor->name(),
                   source.empty() ? "<external>" : source.c_str());
            return;
        }
        const auto* descriptor_name = descriptor->name();
        data_type_descriptors_[id] = std::move(descriptor);
        data_hub_.data_names().emplace(id, descriptor_name);
        printf("[INFO] [Scheduler] Data type descriptor %u:%s registered\n", id, descriptor_name);
    }

    auto init_data_hub() -> bool {
        for (const auto& [id, descriptor] : data_type_descriptors_) {
            if (descriptor == nullptr) {
                printf("[Scheduler] [Error] Data type %u descriptor is null\n", id);
                return false;
            }
            if (!descriptor->install_into(data_hub_)) {
                printf("[Scheduler] [Error] Failed to create data buffer for data type %u:%s\n",
                       id, descriptor->name());
                return false;
            }
            printf("[Scheduler] Data buffer for data type %u:%s created\n",
                   id, descriptor->name());
        }
        return true;
    }

    auto start() -> bool {
        if (running_.load()) {
            return false;
        }
        if (!build_static_info()) {
            return false;
        }
        if (!init_task_flows()) {
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
        stop_task_flows();
        clear_in_flight();
        scheduler::PerfMonitor::instance().report();  // TODO: 析构时报告统计信息
    }

    // External data input
    auto feed(frame_id_t frame, data_type_id_t id, const void* data) -> bool {
        if (!running_.load()) {
            printf("[ERROR] [Scheduler] Failed to feed data for data type %u:%s, frame %u, Scheduler not running\n",
                   id, data_name(id), frame);
            return false;
        }
        if (external_inputs_.count(id) == 0) {
            printf("[ERROR] [Scheduler] Failed to feed data for data type %u:%s, frame %u"
                   ", data type %u:%s is not external input\n",
                   id, data_name(id), frame, id, data_name(id));
            return false;
        }
        const auto status = data_hub_.write_data(id, frame, data);
        if (status != DataAccessStatus::OK) {
            printf("[Scheduler] [Error] feed failed, data type %u:%s, frame %u, status=%u\n",
                   id, data_name(id), frame,
                   static_cast<unsigned>(status));
            return false;
        }
        on_data_arrived(frame, id);
        return true;
    }

    auto data_name(data_type_id_t id) const -> const char* {
        auto it = data_type_descriptors_.find(id);
        if (it == data_type_descriptors_.end() || it->second == nullptr) {
            return "<unknown>";
        }
        return it->second->name();
    }

private:
    // Task to be dispatched
    struct ReadyItem {
        frame_id_t  frame_id = 0;
        std::string task_name;
    };

    auto build_static_info() -> bool {
        printf("[INFO] [Scheduler] Collecting static info...\n");

        tasks_.clear();
        data_producer_.clear();
        external_inputs_.clear();

        for (const auto& flow : task_flows_) {
            if (!flow || !flow->validate_data_types()) {
                return false;
            }
        }

        if (!build_tasks()) {
            return false;
        }

        if (!build_producer()) {
            return false;
        }

        build_external_inputs();

        printf("[INFO] [Scheduler] Static info collected\n");
        return true;
    }

    auto build_tasks() -> bool {
        for (const auto& flow : task_flows_) {
            if (!flow) {
                continue;
            }

            for (const auto& task : flow->tasks()) {
                if (!task) {
                    continue;
                }

                const std::string& name = task->name();
                if (tasks_.find(name) != tasks_.end()) {
                    printf("[ERROR] [Scheduler] Task '%s' already exists\n", name.c_str());
                    return false;
                }

                tasks_[name] = task.get();
                printf("[INFO] [Scheduler] Found task '%s'\n", name.c_str());
            }
        }

        return true;
    }

    auto build_producer() -> bool {
        for (const auto& [name, task] : tasks_) {
            if (!task) {
                continue;
            }

            for (const auto& id : task->produces()) {
                if (data_producer_.find(id) != data_producer_.end()) {
                    printf("[ERROR] [Scheduler] Data type %u:%s found multiple producers: task '%s' and '%s'\n",
                           id, data_name(id), data_producer_[id].c_str(), name.c_str());
                    return false;
                }
                data_producer_[id] = name;
                printf("[INFO] [Scheduler] Found data type %u:%s, producer: '%s'\n",
                       id, data_name(id), name.c_str());
            }
        }

        return true;
    }

    auto build_external_inputs() -> bool {
        std::set<data_type_id_t> all_produced;
        std::set<data_type_id_t> all_consumed;

        for (const auto& [name, task] : tasks_) {
            if (!task) {
                continue;
            }
            for (auto id : task->consumes()) {
                all_consumed.insert(id);
            }
            for (auto id : task->produces()) {
                all_produced.insert(id);
            }
        }

        for (const auto& id : all_consumed) {
            if (all_produced.find(id) == all_produced.end()) {
                external_inputs_.insert(id);
                printf("[INFO] [Scheduler] Found data type %u:%s, external input\n",
                       id, data_name(id));
            }
        }

        return true;
    }

    auto init_task_flows() -> bool {
        for (const auto& flow : task_flows_) {
            if (!flow) {
                continue;
            }
            if (!flow->init()) {
                printf("[Scheduler] [Error] Task flow '%s' init failed\n", flow->name().c_str());
                return false;
            }
            printf("[Scheduler] Task flow '%s' initialized\n", flow->name().c_str());
        }
        return true;
    }

    void stop_task_flows() {
        for (const auto& flow : task_flows_) {
            if (!flow) {
                continue;
            }
            flow->stop();
            printf("[Scheduler] Task flow '%s' stopped\n", flow->name().c_str());
        }
    }

    void init_workers() {
        workers_.clear();
        for (const auto& [name, task] : tasks_) {
            if (!task) {
                continue;
            }
            workers_[name] = std::make_unique<Worker>(
                name,
                task,
                std::make_unique<TaskContext>(
                    data_hub_,
                    task->priority() > 0,
                    [this]() {
                        on_release_priority_lock();
                    }),
                [this](const std::string& name, frame_id_t fid, TaskStatus st) {
                    on_task_done(name, fid, st);
                }
            );
        }
    }

    void start_workers() {
        for (const auto& [name, worker] : workers_) {
            if (!worker) {
                continue;
            }
            worker->start_thread();
        }
    }

    void stop_workers() {
        for (const auto& [name, worker] : workers_) {
            if (!worker) {
                continue;
            }
            worker->stop_thread();
        }
    }

    void clear_in_flight() {
        std::lock_guard<std::mutex> lock(in_flight_mutex_);
        in_flight_.clear();
        ready_queue_.clear();
        priority_lock_held_ = false;
    }

    // Find frame instance, if not exists, create it
    auto get_frame_inst_locked(frame_id_t frame_id, bool create_if_not_exists = false) -> FrameInstance* {
        auto it = in_flight_.find(frame_id);
        if (it != in_flight_.end()) {
            return &it->second;
        }

        auto           emp = in_flight_.emplace(frame_id, FrameInstance{});
        FrameInstance* fi  = &emp.first->second;
        for (const auto& kv : tasks_) {
            fi->task_status.emplace(kv.first, TaskStatus::PENDING);
        }
        return fi;
    }

    void mark_skipped_locked(FrameInstance& fi, frame_id_t frame_id) {
        while (true) {
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
            if (!skipped) {
                break;
            }
        }
    }

    void mark_ready_locked(FrameInstance& fi, frame_id_t frame_id) {
        for (auto& kv : fi.task_status) {
            const std::string& name = kv.first;
            if (kv.second != TaskStatus::PENDING) {
                continue;
            }
            Task* task = tasks_.at(name);
            auto& ctx = workers_[name]->task_context();
            if (!task->is_ready(fi, frame_id, ctx)) {
                continue;
            }
            kv.second = TaskStatus::READY;
            ReadyItem item;
            item.frame_id  = frame_id;
            item.task_name = name;
            ready_queue_.emplace(task->priority(), std::move(item));
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

    auto has_failed_dependency(const FrameInstance& fi, const Task* task) const -> bool {
        for (auto id : task->consumes()) {
            if (fi.failed.count(id) > 0) {
                return true;
            }
        }
        return false;
    }

    void mark_task_finished_locked(FrameInstance&     fi,
                                   const std::string& name,
                                   TaskStatus         status) {
        auto sit = fi.task_status.find(name);
        if (sit == fi.task_status.end()) {
            return;
        }

        if (status == TaskStatus::RETRY) {
            sit->second = TaskStatus::PENDING;
            return;
        }

        sit->second = status;
        ++fi.finished;

        Task* task = tasks_.at(name);
        if (status == TaskStatus::COMPLETED) {
            for (auto id : task->produces()) {
                fi.arrived.insert(id);
            }
        } else if (status == TaskStatus::FAILED) {
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

    /**
     * Dispatching related callbacks
     */

    // Triggered when external data arrived
    //  TODO: trigger this when data produced by task
    void on_data_arrived(frame_id_t frame_id, data_type_id_t id) {
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            FrameInstance* fi = get_frame_inst_locked(frame_id, true);
            if (fi == nullptr) {
                return;
            }
            fi->arrived.insert(id);
            mark_skipped_locked(*fi, frame_id);
            mark_ready_locked(*fi, frame_id);
            erase_frame_if_done_locked(frame_id, *fi);
        }
        dispatch();
    }

    // Triggered when a task completed/failed
    void on_task_done(const std::string& name,
                      frame_id_t         frame_id,
                      TaskStatus         status) {
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            FrameInstance* fi = get_frame_inst_locked(frame_id);
            if (fi != nullptr) {
                mark_task_finished_locked(*fi, name, status);
                mark_skipped_locked(*fi, frame_id);
                mark_ready_locked(*fi, frame_id);
                erase_frame_if_done_locked(frame_id, *fi);
            }
        }
        dispatch();
    }

    // Triggered when a priority task finished
    //  1. automatically triggered by Worker when the task is completed
    //  2. manually triggered in Task::run() when priority lock is no longer needed (optional)
    void on_release_priority_lock() {
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            priority_lock_held_ = false;
        }
        dispatch();
    }

    // Dispatch ready tasks to workers
    void dispatch() {
        std::vector<ReadyItem> to_dispatch;
        {
            std::lock_guard<std::mutex> lock(in_flight_mutex_);
            auto it = ready_queue_.begin();
            // Collect all non-priority tasks
            while (it != ready_queue_.end() && it->first == 0) {
                mark_running_locked(it->second);
                to_dispatch.push_back(std::move(it->second));
                it = ready_queue_.erase(it);
            }
            // Collect the highest priority task
            if (!priority_lock_held_ && it != ready_queue_.end()) {
                priority_lock_held_ = true;
                mark_running_locked(it->second);
                to_dispatch.push_back(std::move(it->second));
                ready_queue_.erase(it);
            }
        }
        // Dispatch to workers, no need to hold the lock
        for (auto& item : to_dispatch) {
            auto wit = workers_.find(item.task_name);
            if (wit != workers_.end()) {
                wit->second->enqueue(item.frame_id);
            }
        }
    }

private:
    std::vector<std::unique_ptr<TaskFlow>>         task_flows_;
    std::map<std::string, std::unique_ptr<Worker>> workers_;

    // Static info
    std::map<std::string, Task*>          tasks_;               // task pointers
    std::map<data_type_id_t, std::string> data_producer_;       // data type producer
    std::set<data_type_id_t>              external_inputs_;     // external inputs

    // In-flight info
    std::mutex                          in_flight_mutex_;
    std::map<frame_id_t, FrameInstance> in_flight_;
    std::multimap<uint8_t, ReadyItem>   ready_queue_;
    bool                                priority_lock_held_ = false;

    std::atomic<bool> running_{false};

    std::map<data_type_id_t, std::shared_ptr<const DataTypeDescriptor>> data_type_descriptors_;

    DataHub data_hub_;
};

} // namespace scheduler
