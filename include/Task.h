#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "TaskContext.h"
#include "Type.h"

namespace scheduler {

class Task {
public:
    Task(const std::string& name) : name_(name) {}
    virtual ~Task() = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    const std::string& name() const { return name_; }

    /**
     * 虚函数：具体任务可以重写
     */
    virtual uint8_t priority() const { return 0; }  // 0 表示无优先级，1 以上越小优先级越高
    virtual bool init(TaskContext& ctx) { return true; }  // 工作线程启动时调用一次
    virtual void stop(TaskContext& ctx) {}  // 工作线程停止时调用一次

    /**
     * 纯虚函数：具体任务必须实现
     */
    virtual const std::vector<data_type_id_t>& consumes() const = 0;  // 返回当前任务消费的数据类型
    virtual const std::vector<data_type_id_t>& produces() const = 0;  // 返回当前任务生产的数据类型
    virtual bool run(frame_id_t frame_id, TaskContext& ctx) = 0;  // 工作线程执行期间，每帧调用一次


private:
    std::string name_;
};

} // namespace scheduler
