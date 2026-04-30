#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "Task.h"
#include "TaskContext.h"
#include "Type.h"

namespace scheduler {

class StereoVisionPreprocessTask final : public Task {
public:
    StereoVisionPreprocessTask() : Task("StereoVisionPreprocessTask") {}

    const std::vector<DataTypeId>& consumes() const override {
        static const std::vector<DataTypeId> consumes = {
            DataTypeId::LEFT_IMAGE,
            DataTypeId::RIGHT_IMAGE
        };
        return consumes;
    }

    const std::vector<DataTypeId>& produces() const override {
        static const std::vector<DataTypeId> produces = {
            DataTypeId::LEFT_IMAGE_PROCESSED,
            DataTypeId::RIGHT_IMAGE_PROCESSED
        };
        return produces;
    }

    bool init(TaskContext& ctx) override {
        return true;
    }

    bool run(frame_id_t frame_id, TaskContext& ctx) override {
        cv::Mat left_image;
        cv::Mat right_image;
        if (!ctx.get_data(frame_id, DataTypeId::LEFT_IMAGE, left_image)) {
            return false;
        }
        if (!ctx.get_data(frame_id, DataTypeId::RIGHT_IMAGE, right_image)) {
            return false;
        }

        cv::Mat left_image_processed;
        cv::Mat right_image_processed;
        cv::resize(left_image, left_image_processed, cv::Size(640, 480));
        cv::resize(right_image, right_image_processed, cv::Size(640, 480));

        ctx.set_data(frame_id, DataTypeId::LEFT_IMAGE_PROCESSED, left_image_processed);
        ctx.set_data(frame_id, DataTypeId::RIGHT_IMAGE_PROCESSED, right_image_processed);
        return true;
    }
};

class StereoVisionInferTask final : public Task {
public:
    StereoVisionInferTask() : Task("StereoVisionInferTask") {}

    const std::vector<DataTypeId>& consumes() const override {
        static const std::vector<DataTypeId> consumes = {
            DataTypeId::LEFT_IMAGE_PROCESSED,
            DataTypeId::RIGHT_IMAGE_PROCESSED
        };
        return consumes;
    }

    const std::vector<DataTypeId>& produces() const override {
        static const std::vector<DataTypeId> produces = {
            DataTypeId::DISPARITY_IMAGE
        };
        return produces;
    }

    bool init(TaskContext& ctx) override {
        return true;
    }

    bool run(frame_id_t frame_id, TaskContext& ctx) override {
        cv::Mat left_image_processed;
        cv::Mat right_image_processed;
        if (!ctx.get_data(frame_id, DataTypeId::LEFT_IMAGE_PROCESSED, left_image_processed)) {
            return false;
        }
        if (!ctx.get_data(frame_id, DataTypeId::RIGHT_IMAGE_PROCESSED, right_image_processed)) {
            return false;
        }

        cv::Mat disparity_image;
        // Infer...

        ctx.set_data(frame_id, DataTypeId::DISPARITY_IMAGE, disparity_image);
        return true;
    }

    void stop(TaskContext& ctx) override {
        return;
    }

    uint8_t priority() const override { return 1; }
};

class StereoVisionPostprocessTask final : public Task {
public:
    StereoVisionPostprocessTask() : Task("StereoVisionPostprocessTask") {}

    const std::vector<DataTypeId>& consumes() const override {
        static const std::vector<DataTypeId> consumes = {
            DataTypeId::DISPARITY_IMAGE
        };
        return consumes;
    }

    const std::vector<DataTypeId>& produces() const override {
        static const std::vector<DataTypeId> produces = {
            DataTypeId::POINT_CLOUD
        };
        return produces;
    }

    bool init(TaskContext& ctx) override {
        return true;
    }

    bool run(frame_id_t frame_id, TaskContext& ctx) override {
        cv::Mat disparity_image;
        if (!ctx.get_data(frame_id, DataTypeId::DISPARITY_IMAGE, disparity_image)) {
            return false;
        }

        point_cloud_t point_cloud;
        // Postprocess...

        ctx.set_data(frame_id, DataTypeId::POINT_CLOUD, point_cloud);
        return true;
    }

    void stop(TaskContext& ctx) override {
        return;
    }
};

class StereoVisionFlow final : public TaskFlowTemplate {
public:
    StereoVisionFlow() : TaskFlowTemplate("StereoVisionFlow") {
        register_data<cv::Mat>(DataTypeId::LEFT_IMAGE);
        register_data<cv::Mat>(DataTypeId::RIGHT_IMAGE);
        register_data<cv::Mat>(DataTypeId::LEFT_IMAGE_PROCESSED);
        register_data<cv::Mat>(DataTypeId::RIGHT_IMAGE_PROCESSED);
        register_data<cv::Mat>(DataTypeId::DISPARITY_IMAGE);
        register_data<point_cloud_t>(DataTypeId::POINT_CLOUD);

        add_task(std::make_unique<StereoVisionPreprocessTask>());
        add_task(std::make_unique<StereoVisionInferTask>());
        add_task(std::make_unique<StereoVisionPostprocessTask>());
    }

    ~StereoVisionFlow() override = default;
};

} // namespace scheduler
