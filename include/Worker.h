#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include "TaskContext.h"
#include "Task.h"
#include "Type.h"

namespace scheduler {

class Worker {
public:
    using on_task_done_t = std::function<void(const std::string&, frame_id_t, TaskStatus)>;

    Worker(const std::string& name,
           Task* task,
           std::unique_ptr<TaskContext> task_context,
           on_task_done_t on_task_done = nullptr)
        : name_(name),
          task_(task),
          task_context_(std::move(task_context)),
          on_task_done_(std::move(on_task_done)) {}

    ~Worker() {
        stop();
    }

    // 禁止拷贝/移动：含 std::thread、mutex 与任务队列，语义上不可复制。
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }
        thread_ = std::thread([this] { run_loop(); });
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        condition_variable_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void enqueue(frame_id_t frame_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) {
            return;
        }
        frame_queue_.push(frame_id);
        condition_variable_.notify_one();
    }

    const std::string& name() const { return name_; }

private:
    void run_loop() {
        if (task_context_ == nullptr || task_ == nullptr ||
            !task_->init(*task_context_)) {
            running_.store(false);
            return;
        }

        while (running_.load()) {
            auto frame_id = wait_for_frame();
            if (!frame_id.has_value()) {
                break;
            }
            run_frame(*frame_id);
        }

        task_->stop(*task_context_);
        running_.store(false);
    }

    std::optional<frame_id_t> wait_for_frame() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_variable_.wait(lock, [this] {
            return !frame_queue_.empty() || !running_.load();
        });
        if (!running_.load()) {
            return std::nullopt;
        }

        frame_id_t frame_id = frame_queue_.front();
        frame_queue_.pop();
        return frame_id;
    }

    void run_frame(frame_id_t frame_id) {
        task_context_->hold_priority_lock();
        const bool ok = task_->run(frame_id, *task_context_);
        task_context_->release_priority_lock();
        if (on_task_done_) {
            on_task_done_(name_, frame_id, ok ? TaskStatus::COMPLETED : TaskStatus::FAILED);
        }
    }

    std::string             name_;
    std::thread             thread_;
    std::atomic<bool>       running_{false};

    std::mutex              mutex_;
    std::condition_variable condition_variable_;
    std::queue<frame_id_t>  frame_queue_;

    Task*                        task_;
    std::unique_ptr<TaskContext> task_context_;
    on_task_done_t               on_task_done_;
};

} // namespace scheduler
