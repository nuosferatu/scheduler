#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include "scheduler/PerfMonitor.h"
#include "scheduler/TaskContext.h"
#include "scheduler/Task.h"
#include "scheduler/Type.h"

namespace scheduler {

class Worker {
public:
    using on_task_done_t = std::function<void(const std::string&, frame_id_t, TaskStatus)>;

    Worker(const std::string&           name,
           Task*                        task,
           std::unique_ptr<TaskContext> task_context,
           on_task_done_t               on_task_done = nullptr)
        : name_(name),
          task_(task),
          task_context_(std::move(task_context)),
          on_task_done_(std::move(on_task_done)) {}

    ~Worker() {
        stop_thread();
    }

    Worker(const Worker&)            = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&)                 = delete;
    Worker& operator=(Worker&&)      = delete;

    void start_thread() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }
        thread_ = std::thread([this] { thread_func(); });
    }

    void stop_thread() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        condition_variable_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        printf("[INFO] [Worker] '%s' stopped\n", name_.c_str());
    }

    // Enqueue a frame ID into the worker thread's frame queue
    void enqueue(frame_id_t frame_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) {
            return;
        }
        frame_queue_.push(frame_id);
        condition_variable_.notify_one();
    }

    const std::string& name() const { return name_; }

    auto task_context() -> TaskContext& {
        return *task_context_;
    }

private:
    void thread_func() {
        // Prefer the thread_name declared by the Task subclass (checked at compile time to be <= 15 chars).
        // Fall back to a truncated task name if not provided, to avoid pthread_setname_np failing on overlong names.
        if (task_ != nullptr && task_->thread_name() != nullptr) {
            pthread_setname_np(pthread_self(), task_->thread_name());
        } else {
            pthread_setname_np(pthread_self(), truncate_thread_name(name_).c_str());
        }

        // 1. Init task context
        if (!task_context_ || !task_ || !task_->init(*task_context_)) {
            running_.store(false);
            return;
        }

        printf("[INFO] [Worker] '%s' started\n", name_.c_str());

        // 2. Run loop
        while (running_.load()) {
            auto frame_id = wait_for_frame();
            if (!frame_id.has_value()) {
                break;  // running_ is false
            }
            run_frame(*frame_id);
        }

        // 3. Destroy task context
        task_->stop(*task_context_);
    }

    auto wait_for_frame() -> std::optional<frame_id_t> {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_variable_.wait(lock, [this] {
            return !frame_queue_.empty() || !running_.load();
        });
        
        if (!running_.load()) {
            return std::nullopt;
        }

        auto frame_id = frame_queue_.front();
        frame_queue_.pop();
        return frame_id;
    }

    void run_frame(frame_id_t frame_id) {
        // 1. Hold priority lock, only if priority is required
        task_context_->hold_priority_lock();

        // 2. Run task
        TaskStatus status = TaskStatus::FAILED;
        {
            auto& perf = PerfMonitor::instance();
            perf.mark_frame_checkpoint(frame_id, name_ + "/begin");
            PerfMonitor::ScopedStage task_stage(name_, frame_id);
            status = task_->run(frame_id, *task_context_);
            if (name_ != "PointCloudPostprocess") {
                perf.mark_frame_checkpoint(frame_id, name_);
            }
        }

        // 3. Release priority lock
        task_context_->release_priority_lock();

        // 4. Trigger Scheduler's task done callback
        if (on_task_done_) {
            on_task_done_(name_, frame_id, status);
        }
    }

    std::string       name_;
    std::thread       thread_;
    std::atomic<bool> running_{false};

    std::mutex              mutex_;
    std::condition_variable condition_variable_;
    std::queue<frame_id_t>  frame_queue_;

    Task*                        task_;
    std::unique_ptr<TaskContext> task_context_;
    on_task_done_t               on_task_done_;
};

} // namespace scheduler
