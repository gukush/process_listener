// csv_export.cpp
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include "orchestrator.hpp"

using namespace std::chrono;

static inline double tp_secs(const unified_monitor::Clock::time_point& tp) {
    return duration<double>(tp.time_since_epoch()).count();
}

namespace unified_monitor {

void ChunkTracker::exportToCSV(const std::string& filename) const {
    std::ofstream file(filename);
    file << "chunk_id,task_id,arrival_time,start_time,completion_time,duration_ms,"
            "peak_cpu_percent,peak_mem_mb,peak_gpu_util_percent,avg_power_mw,samples\n";

    std::lock_guard<std::mutex> lk(mx_);
    for (const auto& kv : chunks_) {
        const auto& chunk_id = kv.first;
        const auto& cm = kv.second;

        double t_arr = tp_secs(cm.arrival_time);
        double t_sta = tp_secs(cm.start_time);
        double t_end = tp_secs(cm.completion_time);
        if (t_end <= 0.0) t_end = tp_secs(Clock::now());
        double duration_ms = (t_end - t_arr) * 1000.0;

        double peak_cpu = 0.0;
        double peak_mem_mb = 0.0;
        for (const auto& os : cm.os_samples) {
            peak_cpu = std::max(peak_cpu, os.cpu_percent);
            peak_mem_mb = std::max(peak_mem_mb, os.mem_rss_kb / 1024.0);
        }

        double peak_gpu_util = 0.0;
        double avg_power = 0.0;
        if (!cm.gpu_samples.empty()) {
            double sum_power = 0.0;
            for (const auto& g : cm.gpu_samples) {
                peak_gpu_util = std::max(peak_gpu_util, static_cast<double>(g.gpu_util_percent));
                sum_power += static_cast<double>(g.power_mw);
            }
            avg_power = sum_power / static_cast<double>(cm.gpu_samples.size());
        }

        file << chunk_id << ","
             << cm.task_id << ","
             << std::fixed << std::setprecision(6) << t_arr << ","
             << std::fixed << std::setprecision(6) << t_sta << ","
             << std::fixed << std::setprecision(6) << t_end << ","
             << std::fixed << std::setprecision(3) << duration_ms << ","
             << std::fixed << std::setprecision(2) << peak_cpu << ","
             << std::fixed << std::setprecision(2) << peak_mem_mb << ","
             << std::fixed << std::setprecision(2) << peak_gpu_util << ","
             << std::fixed << std::setprecision(2) << avg_power << ","
             << (cm.os_samples.size() + cm.gpu_samples.size())
             << "\n";
    }
}

} // namespace unified_monitor
