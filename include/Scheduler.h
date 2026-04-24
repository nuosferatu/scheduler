#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "DataHub.h"
#include "Task.h"
#include "TaskContext.h"
#include "Type.h"
#include "Worker.h"

namespace scheduler {

class Scheduler {
public:
    Scheduler() = default;
    ~Scheduler() = default;

    // 含 DataHub / Worker 等不可拷贝成员
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    void init_workers() {
        for (auto& task_flow_template : task_flow_templates_) {
            for (const auto& task_spec : task_flow_template->task_specs()) {
                TaskContext task_context(data_hub_, task_spec.consumes(), task_spec.produces());
                workers_[task_spec.name()] =
                    std::make_unique<Worker>(task_spec.name(), task_context, task_spec.task_func());
            }
        }
    }

private:
    DataHub                                              data_hub_;
    std::vector<std::unique_ptr<TaskFlowTemplate>>       task_flow_templates_;
    std::map<std::string, std::unique_ptr<Worker>>       workers_;
};

} // namespace scheduler
