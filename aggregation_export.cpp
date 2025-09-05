// aggregation_export.cpp
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include "orchestrator.cpp" // pragma once; brings in ChunkMetrics, MetricsAggregator, structs

using namespace std::chrono;

namespace unified_monitor {

static inline double tp_secs(const Clock::time_point& tp) {
    return duration<double>(tp.time_since_epoch()).count();
}

MetricsAggregator::ChunkAggregates
MetricsAggregator::aggregateChunk(const ChunkMetrics& chunk) {
    ChunkAggregates out{};
    out.chunk_id = chunk.chunk_id;
    out.task_id  = chunk.task_id;

    double t0 = tp_secs(chunk.arrival_time);
    double t1 = tp_secs(chunk.completion_time);
    if (t1 <= 0.0) t1 = tp_secs(Clock::now());
    out.duration_ms = (t1 - t0) * 1000.0;

    // OS
    out.peak_cpu_percent = 0.0;
    out.peak_mem_mb = 0.0;
    for (const auto& os : chunk.os_samples) {
        out.peak_cpu_percent = std::max(out.peak_cpu_percent, os.cpu_percent);
        out.peak_mem_mb = std::max(out.peak_mem_mb, os.mem_rss_kb / 1024.0);
    }

    // GPU
    out.peak_gpu_util_percent = 0.0;
    double sum_power = 0.0;
    for (const auto& g : chunk.gpu_samples) {
        out.peak_gpu_util_percent = std::max(out.peak_gpu_util_percent, static_cast<double>(g.gpu_util_percent));
        sum_power += static_cast<double>(g.power_mw);
    }
    out.avg_power_mw = chunk.gpu_samples.empty() ? 0.0 : (sum_power / chunk.gpu_samples.size());
    out.sample_count = chunk.os_samples.size() + chunk.gpu_samples.size();
    return out;
}

MetricsAggregator::TaskAggregates
MetricsAggregator::aggregateTask(const std::string& task_id,
                                 const std::vector<ChunkMetrics>& chunks) {
    TaskAggregates out{};
    out.task_id = task_id;
    out.chunk_count = 0;
    out.total_duration_ms = 0.0;
    out.total_compute_time_ms = 0.0;
    out.peak_cpu_percent = 0.0;
    out.peak_gpu_util_percent = 0.0;
    out.avg_power_mw = 0.0;

    double sum_power = 0.0;
    size_t power_samples = 0;

    for (const auto& ch : chunks) {
        if (ch.task_id != task_id) continue;
        out.chunk_count++;

        double t0 = tp_secs(ch.arrival_time);
        double t1 = tp_secs(ch.completion_time);
        if (t1 <= 0.0) t1 = tp_secs(Clock::now());
        double dur_ms = (t1 - t0) * 1000.0;
        out.total_duration_ms += dur_ms;

        // naive "compute time" approximation: time between start and completion
        double ts = tp_secs(ch.start_time);
        out.total_compute_time_ms += std::max(0.0, (t1 - ts) * 1000.0);

        for (const auto& os : ch.os_samples) {
            out.peak_cpu_percent = std::max(out.peak_cpu_percent, os.cpu_percent);
        }
        for (const auto& g : ch.gpu_samples) {
            out.peak_gpu_util_percent = std::max(out.peak_gpu_util_percent, static_cast<double>(g.gpu_util_percent));
            sum_power += static_cast<double>(g.power_mw);
            power_samples++;
        }
    }
    out.avg_power_mw = power_samples ? (sum_power / power_samples) : 0.0;
    return out;
}

void MetricsAggregator::exportChunkCSV(const std::string& filename,
                                       const std::vector<ChunkAggregates>& chunks) {
    std::ofstream file(filename);
    file << "chunk_id,task_id,duration_ms,peak_cpu_percent,peak_mem_mb,peak_gpu_util_percent,avg_power_mw,sample_count\n";
    for (const auto& c : chunks) {
        file << c.chunk_id << "," << c.task_id << ","
             << c.duration_ms << "," << c.peak_cpu_percent << ","
             << c.peak_mem_mb << "," << c.peak_gpu_util_percent << ","
             << c.avg_power_mw << "," << c.sample_count << "\n";
    }
}

void MetricsAggregator::exportTaskCSV(const std::string& filename,
                                      const std::vector<TaskAggregates>& tasks) {
    std::ofstream file(filename);
    file << "task_id,chunk_count,total_duration_ms,total_compute_time_ms,peak_cpu_percent,peak_gpu_util_percent,avg_power_mw\n";
    for (const auto& t : tasks) {
        file << t.task_id << "," << t.chunk_count << ","
             << t.total_duration_ms << "," << t.total_compute_time_ms << ","
             << t.peak_cpu_percent << "," << t.peak_gpu_util_percent << ","
             << t.avg_power_mw << "\n";
    }
}

// Optional: dump regular time series for a single chunk (for debugging/plotting)
void MetricsAggregator::exportTimeSeriesCSV(const std::string& filename,
                                            const ChunkMetrics& chunk) {
    std::ofstream f(filename);
    f << "ts,kind,pid,value\n";
    for (const auto& os : chunk.os_samples) {
        f << os.timestamp << ",cpu," << os.pid << "," << os.cpu_percent << "\n";
        f << os.timestamp << ",rss_kb," << os.pid << "," << os.mem_rss_kb << "\n";
    }
    for (const auto& g : chunk.gpu_samples) {
        f << g.timestamp << ",gpu_util," << -1 << "," << static_cast<int>(g.gpu_util_percent) << "\n";
        f << g.timestamp << ",power_mw," << -1 << "," << g.power_mw << "\n";
    }
}

} // namespace unified_monitor
