#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include "TaskContext.h"
#include "Type.h"

namespace scheduler {

class Worker {
public:
    Worker(std::string name,
           TaskContext& task_context,
           task_func_t task_func)
        : name_(std::move(name)),
          task_func_(std::move(task_func)),
          task_context_(std::move(task_context)) {}

    ~Worker() {
        stop();
    }

    // Cannot copy or move, because it contains thread/mutex/condition_variable
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    void start() {
        if (running_.load()) {
            return;
        }
        running_.store(true);
        thread_ = std::thread([this] {
            while (running_.load()) {
                frame_id_t frame_id = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    condition_variable_.wait(lock, [this] {
                        return !frame_queue_.empty() || !running_.load();
                    });
                    if (!running_.load()) {
                        break;
                    }
                    frame_id = frame_queue_.front();
                    frame_queue_.pop();
                }
                (void)frame_id;  // TODO: pass frame_id to task_func_ once supported
                if (!task_func_(task_context_)) {
                    continue;
                }
            }
        });
    }

    void stop() {
        if (!running_.load()) {
            return;
        }
        running_.store(false);
        condition_variable_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    const std::string& name() const { return name_; }

private:
    std::string             name_;
    std::thread             thread_;
    std::atomic<bool>       running_{false};

    std::mutex              mutex_;
    std::condition_variable condition_variable_;
    std::queue<frame_id_t>  frame_queue_;

    task_func_t             task_func_;
    TaskContext             task_context_;
};

} // namespace scheduler
