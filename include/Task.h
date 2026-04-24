#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/opencv.hpp>

#include "DataHub.h"
#include "TaskContext.h"
#include "Type.h"

namespace scheduler {

class TaskSpec {
public:
    TaskSpec(std::string name,
             uint8_t priority,
             std::vector<DataTypeId> consumes,
             std::vector<DataTypeId> produces,
             task_func_t task_func)
        : name_(std::move(name)),
          priority_(priority),
          consumes_(std::move(consumes)),
          produces_(std::move(produces)),
          task_func_(std::move(task_func)) {}

    ~TaskSpec() = default;

    const std::string&              name() const      { return name_; }
    uint8_t                         priority() const  { return priority_; }
    const std::vector<DataTypeId>&  consumes() const  { return consumes_; }
    const std::vector<DataTypeId>&  produces() const  { return produces_; }
    const task_func_t&              task_func() const { return task_func_; }

private:
    std::string             name_;
    uint8_t                 priority_;
    std::vector<DataTypeId> consumes_;
    std::vector<DataTypeId> produces_;
    task_func_t             task_func_;
};

class TaskNode {
public:
    explicit TaskNode(TaskSpec& task_spec) : task_spec_(task_spec) {}
    ~TaskNode() = default;

private:
    frame_id_t  frame_id_ = 0;
    // task_node_id_t task_node_id_ = 0;
    TaskSpec&   task_spec_;
    TaskStatus  status_ = TaskStatus::INITIALIZED;
};

// 任务流模板：抽象基类
// 具体的任务流（如 StereoVisionFlow、SegmentationFlow 等）必须继承本类，
// 并实现 declare_data_types() / declare_tasks() 这两个纯虚钩子。
// 外部统一通过 build() 触发初始化，build() 的调用顺序由基类固定，保证
// 数据类型先于任务被声明。
class TaskFlowTemplate {
public:
    explicit TaskFlowTemplate(std::string name) : name_(std::move(name)) {}

    // 多态基类必须 virtual dtor
    virtual ~TaskFlowTemplate() = default;

    // 抽象基类通常不允许拷贝/赋值，避免对象切片
    TaskFlowTemplate(const TaskFlowTemplate&) = delete;
    TaskFlowTemplate& operator=(const TaskFlowTemplate&) = delete;

    // Template Method：骨架流程由基类固定，子类只负责填内容
    virtual void build() final {
        declare_data_types();
        declare_tasks();
    }

    const std::string&                   name() const             { return name_; }
    const std::vector<TaskSpec>&         task_specs() const       { return task_specs_; }
    const std::vector<data_type_info_t>& data_type_infos() const  { return data_type_infos_; }

protected:
    // 子类声明该任务流用到的 DataTypeId（填充 data_type_infos_）
    virtual void declare_data_types() = 0;

    // 子类声明该任务流包含的 TaskSpec（填充 task_specs_）
    virtual void declare_tasks() = 0;

    std::string                   name_;
    std::vector<TaskSpec>         task_specs_;
    std::vector<data_type_info_t> data_type_infos_;
};

class StereoVisionFlow : public TaskFlowTemplate {
public:
    StereoVisionFlow() : TaskFlowTemplate("StereoVisionFlow") {}
    ~StereoVisionFlow() override = default;

protected:
    void declare_data_types() override {
        data_type_infos_.push_back({DataTypeId::LEFT_IMAGE,       2, sizeof(cv::Mat), nullptr});
        data_type_infos_.push_back({DataTypeId::RIGHT_IMAGE,      2, sizeof(cv::Mat), nullptr});
        data_type_infos_.push_back({DataTypeId::DISPARITY_IMAGE,  2, sizeof(cv::Mat), nullptr});
        data_type_infos_.push_back({DataTypeId::POINT_CLOUD,      2, sizeof(cv::Mat), nullptr});
    }

    void declare_tasks() override {
        task_specs_.push_back(TaskSpec{
            "StereoImagesPreprocess",
            0,
            {
                DataTypeId::LEFT_IMAGE,
                DataTypeId::RIGHT_IMAGE
            },
            {
                DataTypeId::DISPARITY_IMAGE,
                DataTypeId::POINT_CLOUD
            },
            [](TaskContext& task_context) -> bool {
                frame_id_t frame_id = 0;
                cv::Mat left_image;
                cv::Mat right_image;
                if (!task_context.get_data(frame_id, DataTypeId::LEFT_IMAGE, left_image)) {
                    return false;
                }
                if (!task_context.get_data(frame_id, DataTypeId::RIGHT_IMAGE, right_image)) {
                    return false;
                }
                return true;
            }
        });
    }
};

} // namespace scheduler
