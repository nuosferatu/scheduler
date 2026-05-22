#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "scheduler/Type.h"

namespace scheduler {

class StageMonitor {
public:
    using clock_t      = std::chrono::steady_clock;
    using time_point_t = clock_t::time_point;
    using stage_key_t  = std::pair<frame_id_t, std::string>;

    StageMonitor()                               = default;
    ~StageMonitor()                              = default;
    StageMonitor(const StageMonitor&)            = delete;
    StageMonitor& operator=(const StageMonitor&) = delete;
    StageMonitor(StageMonitor&&)                 = delete;
    StageMonitor& operator=(StageMonitor&&)      = delete;

    void begin(const std::string& name, frame_id_t frame_id) {
        const auto now = clock_t::now();
        std::lock_guard<std::mutex> lock(mutex_);

        // 名字含 '/' 视为子阶段，直接父必须处于活跃状态，否则拒绝
        const auto sep = name.rfind('/');
        if (sep != std::string::npos) {
            const auto parent = name.substr(0, sep);
            if (active_.find(stage_key_t{frame_id, parent}) == active_.end()) {
                printf("[PerfMonitor] [Warn] sub-stage '%s' frame %u rejected: "
                       "parent '%s' is not active\n",
                       name.c_str(), frame_id, parent.c_str());
                return;
            }
        }

        active_.emplace(stage_key_t{frame_id, name}, now);  // 重复调用会被忽略
    }

    void end(const std::string& name, frame_id_t frame_id) {
        const auto now = clock_t::now();
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_.find(stage_key_t{frame_id, name});
        if (it == active_.end()) {
            // 没有对应的 begin，直接返回
            return;
        }

        // 父结束时自动结算并 erase 所有还活着的后代：
        // 同 frame_id 内 key 按字典序连续排列，所有后代共享前缀 "<name>/"，
        // 且 "name" < "name/"，因此 it 不在被 erase 的范围内，保持有效。
        const std::string prefix = name + '/';
        auto child_it = active_.lower_bound(stage_key_t{frame_id, prefix});
        while (child_it != active_.end()
               && child_it->first.first == frame_id
               && child_it->first.second.compare(0, prefix.size(), prefix) == 0) {
            const auto& child_name = child_it->first.second;
            const auto  child_elapsed_ms = std::chrono::duration<double, std::milli>(
                                               now - child_it->second).count();
            record_stat_locked(child_name, child_elapsed_ms);
            printf("[PerfMonitor] [Warn] sub-stage '%s' frame %u auto-ended by parent '%s'\n",
                   child_name.c_str(), frame_id, name.c_str());
            child_it = active_.erase(child_it);
        }

        const auto elapsed_ms = std::chrono::duration<double, std::milli>(
                                    now - it->second).count();
        active_.erase(it);
        record_stat_locked(name, elapsed_ms);
    }

    void report() const {
        std::lock_guard<std::mutex> lock(mutex_);
        printf("[PerfMonitor] ===== stage elapsed report =====\n");
        printf("[PerfMonitor] %-32s %10s %10s %10s %10s %10s\n",
               "stage", "count", "last(ms)", "mean(ms)", "min(ms)", "max(ms)");
        for (const auto& [name, stat] : stats_) {
            const auto mean = stat.count == 0
                                  ? 0.0
                                  : stat.sum_ms / static_cast<double>(stat.count);
            // 按 '/' 层级缩进显示，仅展示最末段名字，更紧凑
            const auto depth = static_cast<int>(std::count(name.begin(), name.end(), '/'));
            const auto leaf_pos = name.rfind('/');
            const auto leaf = leaf_pos == std::string::npos
                                  ? name
                                  : name.substr(leaf_pos + 1);
            printf("[PerfMonitor] %*s%-*s %10llu %10.3f %10.3f %10.3f %10.3f\n",
                   depth * 2, "",
                   32 - depth * 2, leaf.c_str(),
                   static_cast<unsigned long long>(stat.count),
                   stat.last_ms, mean, stat.min_ms, stat.max_ms);
        }
        if (!active_.empty()) {
            printf("[PerfMonitor] [Warn] %zu stage(s) still active and not ended:\n",
                   active_.size());
            for (const auto& [key, start] : active_) {
                (void)start;
                printf("[PerfMonitor] [Warn]   frame=%u stage='%s'\n",
                       key.first, key.second.c_str());
            }
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.clear();
        stats_.clear();
    }

private:
    struct Stat {
        uint64_t count   = 0;
        double   sum_ms  = 0.0;
        double   min_ms  = 0.0;
        double   max_ms  = 0.0;
        double   last_ms = 0.0;
    };

    // 调用方需持有 mutex_
    void record_stat_locked(const std::string& name, double elapsed_ms) {
        auto& stat = stats_[name];
        stat.last_ms = elapsed_ms;
        stat.sum_ms += elapsed_ms;
        if (stat.count == 0) {
            stat.min_ms = elapsed_ms;
            stat.max_ms = elapsed_ms;
        } else {
            stat.min_ms = std::min(stat.min_ms, elapsed_ms);
            stat.max_ms = std::max(stat.max_ms, elapsed_ms);
        }
        ++stat.count;
    }

    mutable std::mutex                  mutex_;
    std::map<stage_key_t, time_point_t> active_;  // 活跃的阶段
    std::map<std::string, Stat>         stats_;   // 阶段耗时统计
};

class BufferActivityMonitor {
public:
    using clock_t             = std::chrono::steady_clock;
    using buffer_frame_key_t  = std::pair<std::string, frame_id_t>;

    BufferActivityMonitor()                                    = default;
    ~BufferActivityMonitor()                                   = default;
    BufferActivityMonitor(const BufferActivityMonitor&)        = delete;
    BufferActivityMonitor& operator=(const BufferActivityMonitor&) = delete;
    BufferActivityMonitor(BufferActivityMonitor&&)             = delete;
    BufferActivityMonitor& operator=(BufferActivityMonitor&&)  = delete;

    // DataBuffer hooks; no-op when buffer_name is empty
    void on_insert(const std::string& buffer_name, frame_id_t frame_id) {
        if (buffer_name.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        insert_times_[buffer_frame_key_t{buffer_name, frame_id}] = clock_t::now();
    }

    void on_first_read(const std::string& buffer_name, frame_id_t frame_id) {
        if (buffer_name.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = buffer_frame_key_t{buffer_name, frame_id};
        const auto it  = insert_times_.find(key);
        if (it == insert_times_.end()) {
            return;
        }
        const auto latency_ms = std::chrono::duration<double, std::milli>(
                                    clock_t::now() - it->second).count();
        insert_times_.erase(it);
        record_insert_to_first_read_locked(buffer_name, frame_id, latency_ms);
    }

    void on_cancel_insert_wait(const std::string& buffer_name, frame_id_t frame_id) {
        if (buffer_name.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        insert_times_.erase(buffer_frame_key_t{buffer_name, frame_id});
    }

    void on_evicted_before_read(const std::string& buffer_name, frame_id_t frame_id) {
        if (buffer_name.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (insert_times_.erase(buffer_frame_key_t{buffer_name, frame_id}) == 0) {
            return;
        }
        record_evicted_before_read_locked(buffer_name, frame_id);
    }

    void on_size_snap(const std::string& buffer_name,
                      size_t             capacity,
                      size_t&            size_snap,
                      size_t             new_size,
                      const char*        op) {
        if (new_size == size_snap) {
            return;
        }
        on_size_changed(buffer_name, capacity, size_snap, new_size, op);
        size_snap = new_size;
    }

    void on_size_changed(const std::string& buffer_name,
                         size_t             capacity,
                         size_t             old_size,
                         size_t             new_size,
                         const char*        op) {
        if (buffer_name.empty() || old_size == new_size) {
            return;
        }
        const auto usage = capacity > 0
                               ? static_cast<double>(new_size) / static_cast<double>(capacity)
                               : 0.0;

        std::lock_guard<std::mutex> lock(mutex_);
        auto& stat = usage_stats_[buffer_name];
        stat.capacity     = capacity;
        stat.current_size = new_size;
        stat.last_usage   = usage;
        stat.sum_usage += usage;
        if (stat.change_count == 0) {
            stat.min_usage = usage;
            stat.max_usage = usage;
        } else {
            stat.min_usage = std::min(stat.min_usage, usage);
            stat.max_usage = std::max(stat.max_usage, usage);
        }
        ++stat.change_count;

        if (kReportBufferUsageEvents) {
            const auto now = clock_t::now();
            const auto steady_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now.time_since_epoch()).count();
            const auto kind = new_size > old_size ? SizeChangeKind::GROW
                                                  : SizeChangeKind::SHRINK;
            UsageEvent ev;
            ev.steady_ms   = steady_ms;
            ev.old_size    = old_size;
            ev.new_size    = new_size;
            ev.usage_ratio = usage;
            ev.kind        = kind;
            ev.op          = op;
            stat.events.push_back(ev);
            while (stat.events.size() > kMaxBufferUsageEvents) {
                stat.events.pop_front();
            }
        }
    }

    void report() const {
        std::lock_guard<std::mutex> lock(mutex_);
        report_usage_locked();
        report_insert_to_first_read_locked();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        usage_stats_.clear();
        first_read_stats_.clear();
        insert_times_.clear();
    }

private:
    enum class SizeChangeKind : uint8_t {
        GROW   = 0,
        SHRINK = 1,
    };

    struct UsageEvent {
        int64_t        steady_ms   = 0;
        size_t         old_size    = 0;
        size_t         new_size    = 0;
        double         usage_ratio = 0.0;
        SizeChangeKind kind        = SizeChangeKind::GROW;
        const char*    op          = nullptr;
    };

    struct UsageStat {
        size_t   capacity     = 0;
        size_t   current_size = 0;
        uint64_t change_count = 0;
        double   sum_usage    = 0.0;
        double   min_usage    = 0.0;
        double   max_usage    = 0.0;
        double   last_usage   = 0.0;
        std::deque<UsageEvent> events;
    };

    // insert -> first read_lease latency per buffer
    struct InsertToFirstReadStat {
        uint64_t sample_count        = 0;
        uint64_t evicted_before_read = 0;
        double   sum_ms              = 0.0;
        double   min_ms              = 0.0;
        double   max_ms              = 0.0;
        double   last_ms             = 0.0;
    };

    static constexpr size_t  kMaxBufferUsageEvents  = 4096;
    static constexpr bool    kReportBufferUsageEvents = false;  // 逐条 size 变化事件，日志过多时可关

    void record_insert_to_first_read_locked(const std::string& buffer_name,
                                            frame_id_t           frame_id,
                                            double               latency_ms) {
        (void)frame_id;
        auto& stat = first_read_stats_[buffer_name];
        stat.last_ms = latency_ms;
        stat.sum_ms += latency_ms;
        if (stat.sample_count == 0) {
            stat.min_ms = latency_ms;
            stat.max_ms = latency_ms;
        } else {
            stat.min_ms = std::min(stat.min_ms, latency_ms);
            stat.max_ms = std::max(stat.max_ms, latency_ms);
        }
        ++stat.sample_count;
    }

    void record_evicted_before_read_locked(const std::string& buffer_name,
                                           frame_id_t           frame_id) {
        (void)frame_id;
        ++first_read_stats_[buffer_name].evicted_before_read;
    }

    void report_usage_locked() const {
        if (usage_stats_.empty()) {
            return;
        }
        printf("[PerfMonitor] ===== buffer usage report =====\n");
        printf("[PerfMonitor] %-28s %8s %8s %10s %8s %8s %8s %8s\n",
               "buffer", "capacity", "current", "changes",
               "last(%)", "mean(%)", "min(%)", "max(%)");
        for (const auto& [name, stat] : usage_stats_) {
            const auto mean = stat.change_count == 0
                                  ? 0.0
                                  : stat.sum_usage / static_cast<double>(stat.change_count);
            printf("[PerfMonitor] %-28s %8zu %8zu %10llu %8.1f %8.1f %8.1f %8.1f\n",
                   name.c_str(),
                   stat.capacity,
                   stat.current_size,
                   static_cast<unsigned long long>(stat.change_count),
                   stat.last_usage * 100.0,
                   mean * 100.0,
                   stat.min_usage * 100.0,
                   stat.max_usage * 100.0);
        }
        if (kReportBufferUsageEvents) {
            for (const auto& [name, stat] : usage_stats_) {
                if (stat.events.empty()) {
                    continue;
                }
                printf("[PerfMonitor] ----- buffer usage events: %s (%zu) -----\n",
                       name.c_str(), stat.events.size());
                for (const auto& ev : stat.events) {
                    const auto* kind_str = ev.kind == SizeChangeKind::GROW ? "grow" : "shrink";
                    const auto* op_str   = ev.op != nullptr ? ev.op : "-";
                    printf("[PerfMonitor]   t=%lld %s %-12s size %zu->%zu usage %.1f%%\n",
                           static_cast<long long>(ev.steady_ms),
                           kind_str,
                           op_str,
                           ev.old_size,
                           ev.new_size,
                           ev.usage_ratio * 100.0);
                }
            }
        }
        printf("[PerfMonitor] ===== buffer usage end =====\n");
    }

    void report_insert_to_first_read_locked() const {
        if (first_read_stats_.empty()) {
            return;
        }
        printf("[PerfMonitor] ===== buffer insert->first-read latency =====\n");
        printf("[PerfMonitor] %-28s %10s %10s %10s %10s %10s %12s\n",
               "buffer", "samples", "last(ms)", "mean(ms)", "min(ms)", "max(ms)",
               "evict_no_rd");
        for (const auto& [name, stat] : first_read_stats_) {
            const auto mean = stat.sample_count == 0
                                  ? 0.0
                                  : stat.sum_ms / static_cast<double>(stat.sample_count);
            printf("[PerfMonitor] %-28s %10llu %10.3f %10.3f %10.3f %10.3f %12llu\n",
                   name.c_str(),
                   static_cast<unsigned long long>(stat.sample_count),
                   stat.last_ms,
                   mean,
                   stat.min_ms,
                   stat.max_ms,
                   static_cast<unsigned long long>(stat.evicted_before_read));
        }
        printf("[PerfMonitor] ===== buffer insert->first-read end =====\n");
    }

    mutable std::mutex                                 mutex_;
    std::map<std::string, UsageStat>                   usage_stats_;
    std::map<std::string, InsertToFirstReadStat>       first_read_stats_;
    std::map<buffer_frame_key_t, clock_t::time_point>    insert_times_;
};

// End-to-end frame latency: dispatch entry -> point cloud callback (exclusive).
class FramePipelineMonitor {
public:
    using clock_t      = std::chrono::steady_clock;
    using time_point_t = clock_t::time_point;

    static constexpr const char* kTotalMetric = "dispatch->callback";

    FramePipelineMonitor()                                    = default;
    ~FramePipelineMonitor()                                   = default;
    FramePipelineMonitor(const FramePipelineMonitor&)         = delete;
    FramePipelineMonitor& operator=(const FramePipelineMonitor&) = delete;
    FramePipelineMonitor(FramePipelineMonitor&&)              = delete;
    FramePipelineMonitor& operator=(FramePipelineMonitor&&)   = delete;

    void begin(frame_id_t frame_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.insert_or_assign(frame_id, FrameRecord{clock_t::now(), {}});
    }

    void mark(frame_id_t frame_id, const std::string& checkpoint) {
        const auto now = clock_t::now();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = active_.find(frame_id);
        if (it == active_.end()) {
            return;
        }
        const auto elapsed_ms = std::chrono::duration<double, std::milli>(
                                    now - it->second.start).count();
        it->second.checkpoints.emplace_back(checkpoint, elapsed_ms);
        record_checkpoint_stat_locked(checkpoint, elapsed_ms);
    }

    void finish(frame_id_t frame_id) {
        const auto now = clock_t::now();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = active_.find(frame_id);
        if (it == active_.end()) {
            return;
        }

        const auto total_ms = std::chrono::duration<double, std::milli>(
                                  now - it->second.start).count();
        record_total_stat_locked(total_ms);
        record_checkpoint_stat_locked("PointCloudPostprocess/pre_callback", total_ms);
        it->second.checkpoints.emplace_back("PointCloudPostprocess/pre_callback", total_ms);
        last_frame_ = LastFrameDetail{frame_id, total_ms, it->second.checkpoints};
        active_.erase(it);
    }

    void cancel(frame_id_t frame_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.erase(frame_id);
    }

    void report() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (total_stat_.count == 0 && checkpoint_stats_.empty()) {
            return;
        }

        printf("[PerfMonitor] ===== frame pipeline (dispatch -> callback) =====\n");
        printf("[PerfMonitor] %-32s %10s %10s %10s %10s %10s\n",
               "metric", "count", "last(ms)", "mean(ms)", "min(ms)", "max(ms)");
        if (total_stat_.count > 0) {
            const auto mean = total_stat_.sum_ms / static_cast<double>(total_stat_.count);
            printf("[PerfMonitor] %-32s %10llu %10.3f %10.3f %10.3f %10.3f\n",
                   kTotalMetric,
                   static_cast<unsigned long long>(total_stat_.count),
                   total_stat_.last_ms,
                   mean,
                   total_stat_.min_ms,
                   total_stat_.max_ms);
        }

        printf("[PerfMonitor] ----- checkpoint elapsed from dispatch start -----\n");
        printf("[PerfMonitor] %-32s %10s %10s %10s %10s %10s\n",
               "checkpoint", "count", "last(ms)", "mean(ms)", "min(ms)", "max(ms)");
        static constexpr const char* kCheckpointOrder[] = {
            "Dispatch",
            "StereoPreprocess/begin",
            "StereoPreprocess",
            "IgevInfer/begin",
            "IgevInfer",
            "PointCloudPostprocess/begin",
            "PointCloudPostprocess/pre_callback",
        };
        for (const auto* name : kCheckpointOrder) {
            const auto it = checkpoint_stats_.find(name);
            if (it == checkpoint_stats_.end()) {
                continue;
            }
            const auto& stat = it->second;
            const auto mean = stat.count == 0
                                  ? 0.0
                                  : stat.sum_ms / static_cast<double>(stat.count);
            printf("[PerfMonitor] %-32s %10llu %10.3f %10.3f %10.3f %10.3f\n",
                   name,
                   static_cast<unsigned long long>(stat.count),
                   stat.last_ms,
                   mean,
                   stat.min_ms,
                   stat.max_ms);
        }

        if (last_frame_.has_value()) {
            const auto& detail = *last_frame_;
            printf("[PerfMonitor] ----- last completed frame %u (total %.3f ms) -----\n",
                   detail.frame_id,
                   detail.total_ms);
            for (const auto& [name, elapsed_ms] : detail.checkpoints) {
                printf("[PerfMonitor]   %-32s %10.3f ms\n", name.c_str(), elapsed_ms);
            }
        }

        if (!active_.empty()) {
            printf("[PerfMonitor] [Warn] %zu frame pipeline(s) still active:\n", active_.size());
            for (const auto& [frame_id, record] : active_) {
                (void)record;
                printf("[PerfMonitor] [Warn]   frame=%u\n", frame_id);
            }
        }
        printf("[PerfMonitor] ===== frame pipeline end =====\n");
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.clear();
        checkpoint_stats_.clear();
        total_stat_ = {};
        last_frame_.reset();
    }

private:
    struct Stat {
        uint64_t count   = 0;
        double   sum_ms  = 0.0;
        double   min_ms  = 0.0;
        double   max_ms  = 0.0;
        double   last_ms = 0.0;
    };

    struct FrameRecord {
        time_point_t start;
        std::vector<std::pair<std::string, double>> checkpoints;
    };

    struct LastFrameDetail {
        frame_id_t                                  frame_id;
        double                                      total_ms;
        std::vector<std::pair<std::string, double>> checkpoints;
    };

    void record_total_stat_locked(double elapsed_ms) {
        total_stat_.last_ms = elapsed_ms;
        total_stat_.sum_ms += elapsed_ms;
        if (total_stat_.count == 0) {
            total_stat_.min_ms = elapsed_ms;
            total_stat_.max_ms = elapsed_ms;
        } else {
            total_stat_.min_ms = std::min(total_stat_.min_ms, elapsed_ms);
            total_stat_.max_ms = std::max(total_stat_.max_ms, elapsed_ms);
        }
        ++total_stat_.count;
    }

    void record_checkpoint_stat_locked(const std::string& checkpoint, double elapsed_ms) {
        auto& stat = checkpoint_stats_[checkpoint];
        stat.last_ms = elapsed_ms;
        stat.sum_ms += elapsed_ms;
        if (stat.count == 0) {
            stat.min_ms = elapsed_ms;
            stat.max_ms = elapsed_ms;
        } else {
            stat.min_ms = std::min(stat.min_ms, elapsed_ms);
            stat.max_ms = std::max(stat.max_ms, elapsed_ms);
        }
        ++stat.count;
    }

    mutable std::mutex                           mutex_;
    std::map<frame_id_t, FrameRecord>            active_;
    std::map<std::string, Stat>                  checkpoint_stats_;
    Stat                                         total_stat_;
    std::optional<LastFrameDetail>               last_frame_;
};

class PerfMonitor {
public:
    static auto instance() -> PerfMonitor& {
        static PerfMonitor inst;
        return inst;
    }

    PerfMonitor(const PerfMonitor&)            = delete;
    PerfMonitor& operator=(const PerfMonitor&) = delete;
    PerfMonitor(PerfMonitor&&)                 = delete;
    PerfMonitor& operator=(PerfMonitor&&)      = delete;

    class ScopedStage {
    public:
        ScopedStage(std::string name, frame_id_t frame_id)
            : name_(std::move(name)), frame_id_(frame_id) {
            PerfMonitor::instance().begin_stage(name_, frame_id_);
        }

        ~ScopedStage() {
            PerfMonitor::instance().end_stage(name_, frame_id_);
        }

        ScopedStage(const ScopedStage&)            = delete;
        ScopedStage& operator=(const ScopedStage&) = delete;
        ScopedStage(ScopedStage&&)                 = delete;
        ScopedStage& operator=(ScopedStage&&)      = delete;

    private:
        std::string name_;
        frame_id_t  frame_id_;
    };

    auto stages() -> StageMonitor& { return stage_monitor_; }
    auto buffers() -> BufferActivityMonitor& { return buffer_monitor_; }
    auto frame_pipeline() -> FramePipelineMonitor& { return frame_pipeline_monitor_; }

    auto stages() const -> const StageMonitor& { return stage_monitor_; }
    auto buffers() const -> const BufferActivityMonitor& { return buffer_monitor_; }
    auto frame_pipeline() const -> const FramePipelineMonitor& { return frame_pipeline_monitor_; }

    void begin_stage(const std::string& name, frame_id_t frame_id) {
        stage_monitor_.begin(name, frame_id);
    }

    void end_stage(const std::string& name, frame_id_t frame_id) {
        stage_monitor_.end(name, frame_id);
    }

    void begin_frame_pipeline(frame_id_t frame_id) {
        frame_pipeline_monitor_.begin(frame_id);
    }

    void mark_frame_checkpoint(frame_id_t frame_id, const std::string& checkpoint) {
        frame_pipeline_monitor_.mark(frame_id, checkpoint);
    }

    void finish_frame_pipeline(frame_id_t frame_id) {
        frame_pipeline_monitor_.finish(frame_id);
    }

    void cancel_frame_pipeline(frame_id_t frame_id) {
        frame_pipeline_monitor_.cancel(frame_id);
    }

    void on_buffer_insert(const std::string& buffer_name, frame_id_t frame_id) {
        buffer_monitor_.on_insert(buffer_name, frame_id);
    }

    void on_buffer_first_read(const std::string& buffer_name, frame_id_t frame_id) {
        buffer_monitor_.on_first_read(buffer_name, frame_id);
    }

    void on_buffer_cancel_insert_wait(const std::string& buffer_name, frame_id_t frame_id) {
        buffer_monitor_.on_cancel_insert_wait(buffer_name, frame_id);
    }

    void on_buffer_evicted_before_read(const std::string& buffer_name, frame_id_t frame_id) {
        buffer_monitor_.on_evicted_before_read(buffer_name, frame_id);
    }

    void on_buffer_size_snap(const std::string& buffer_name,
                             size_t             capacity,
                             size_t&            size_snap,
                             size_t             new_size,
                             const char*        op) {
        buffer_monitor_.on_size_snap(buffer_name, capacity, size_snap, new_size, op);
    }

    void on_buffer_size_changed(const std::string& buffer_name,
                                size_t             capacity,
                                size_t             old_size,
                                size_t             new_size,
                                const char*        op) {
        buffer_monitor_.on_size_changed(buffer_name, capacity, old_size, new_size, op);
    }

    void report() const {
        stage_monitor_.report();
        buffer_monitor_.report();
        frame_pipeline_monitor_.report();
        printf("[PerfMonitor] ===== end =====\n");
    }

    void clear() {
        stage_monitor_.clear();
        buffer_monitor_.clear();
        frame_pipeline_monitor_.clear();
    }

private:
    PerfMonitor()  = default;
    ~PerfMonitor() = default;

    StageMonitor          stage_monitor_;
    BufferActivityMonitor buffer_monitor_;
    FramePipelineMonitor  frame_pipeline_monitor_;
};

}    // namespace scheduler
