#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h> // pid_t

namespace unified_monitor {

// A steady clock used for timepoints inside the app
using Clock = std::chrono::steady_clock;

// --------- Metric data structures ---------
struct OSMetrics {
    double     timestamp = 0.0;    // seconds since epoch
    pid_t      pid = -1;

    double     cpu_percent = 0.0;  // optional; 0 if not computed
    long       mem_rss_kb = 0;
    long       mem_vms_kb = 0;

    std::uint64_t disk_read_bytes  = 0;
    std::uint64_t disk_write_bytes = 0;
    std::uint64_t net_recv_bytes   = 0;
    std::uint64_t net_sent_bytes   = 0;
};

struct GPUMetrics {
    double     timestamp = 0.0;
    unsigned   gpu_index = 0;

    unsigned   power_mw = 0;
    int        gpu_util_percent = 0;
    int        mem_util_percent = 0;
    std::uint64_t mem_used_bytes = 0;
    unsigned   sm_clock_mhz = 0;

    // Optional per-PID GPU util (smUtil) %
    std::map<unsigned, int> pid_gpu_percent;
};

// --------- Chunk tracking ---------
struct ChunkMetrics {
    std::string chunk_id;
    std::string task_id;

    Clock::time_point arrival_time{};
    Clock::time_point start_time{};
    Clock::time_point completion_time{};

    std::string status;

    std::vector<OSMetrics>  os_samples;
    std::vector<GPUMetrics> gpu_samples;
};

class ChunkTracker {
public:
    void onChunkArrival(const std::string& chunk_id, const std::string& task_id) {
        std::lock_guard<std::mutex> lk(mx_);
        auto& ch = chunks_[chunk_id];
        ch.chunk_id = chunk_id;
        ch.task_id = task_id;
        ch.arrival_time = Clock::now();
    }
    void onChunkStart(const std::string& chunk_id) {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = chunks_.find(chunk_id);
        if (it != chunks_.end()) it->second.start_time = Clock::now();
    }
    void onChunkComplete(const std::string& chunk_id, const std::string& status) {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = chunks_.find(chunk_id);
        if (it != chunks_.end()) {
            it->second.completion_time = Clock::now();
            it->second.status = status;
        }
    }
    void attachOSMetrics(const std::string& chunk_id, const std::vector<OSMetrics>& os) {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = chunks_.find(chunk_id);
        if (it != chunks_.end()) {
            it->second.os_samples.insert(it->second.os_samples.end(), os.begin(), os.end());
        }
    }
    void attachGPUMetrics(const std::string& chunk_id, const std::vector<GPUMetrics>& gpu) {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = chunks_.find(chunk_id);
        if (it != chunks_.end()) {
            it->second.gpu_samples.insert(it->second.gpu_samples.end(), gpu.begin(), gpu.end());
        }
    }

    std::vector<ChunkMetrics> getAllChunks() const {
        std::lock_guard<std::mutex> lk(mx_);
        std::vector<ChunkMetrics> out;
        out.reserve(chunks_.size());
        for (const auto& kv : chunks_) out.push_back(kv.second);
        return out;
    }

    // Implemented in csv_export.cpp
    void exportToCSV(const std::string& filename) const;

private:
    friend class MetricsAggregator;
    mutable std::mutex mx_;
    std::map<std::string, ChunkMetrics> chunks_;
};

// --------- Collectors ---------
class OSMetricsCollector {
public:
    OSMetricsCollector();
    ~OSMetricsCollector();

    void startMonitoring(const std::vector<pid_t>& pids, unsigned interval_ms);
    void stopMonitoring();
    std::vector<OSMetrics> getMetrics() const;

    // Implemented in os_metrics_linux.cpp
    OSMetrics collectForPid(pid_t pid);

private:
    std::vector<pid_t> monitored_pids_;
    unsigned interval_ms_ = 100;
    std::atomic<bool> running_{false};
    mutable std::mutex metrics_mutex_;
    std::thread monitor_thread_;
    std::vector<OSMetrics> metrics_;
};

class GPUMetricsCollector {
public:
    explicit GPUMetricsCollector(unsigned gpu_index);
    ~GPUMetricsCollector();

    void startMonitoring(unsigned interval_ms);
    void stopMonitoring();
    std::vector<GPUMetrics> getMetrics() const;

private:
    unsigned gpu_index_ = 0;
    unsigned interval_ms_ = 50;
    std::atomic<bool> running_{false};
    mutable std::mutex mx_;
    std::thread worker_;
    std::vector<GPUMetrics> samples_;
};

// --------- Aggregator (decl) ---------
class MetricsAggregator {
public:
    struct ChunkAggregates {
        std::string chunk_id;
        std::string task_id;
        double duration_ms = 0.0;
        double peak_cpu_percent = 0.0;
        double peak_mem_mb = 0.0;
        double peak_gpu_util_percent = 0.0;
        double avg_power_mw = 0.0;
        std::size_t sample_count = 0;
    };

    struct TaskAggregates {
        std::string task_id;
        std::size_t chunk_count = 0;
        double total_duration_ms = 0.0;
        double total_compute_time_ms = 0.0;
        double peak_cpu_percent = 0.0;
        double peak_gpu_util_percent = 0.0;
        double avg_power_mw = 0.0;
    };

    ChunkAggregates aggregateChunk(const ChunkMetrics& chunk);
    TaskAggregates  aggregateTask(const std::string& task_id,
                                  const std::vector<ChunkMetrics>& chunks);

    void exportChunkCSV(const std::string& filename,
                        const std::vector<ChunkAggregates>& chunks);
    void exportTaskCSV(const std::string& filename,
                       const std::vector<TaskAggregates>& tasks);
    void exportTimeSeriesCSV(const std::string& filename,
                             const ChunkMetrics& chunk);
};

// --------- Process command specs for config-driven launch ---------
struct CommandSpec {
    bool enabled = true;                          // if false, skip launching
    bool shell = false;                           // true => run via /bin/sh -lc "cmd"
    std::vector<std::string> argv;                // argv form (takes precedence over cmd)
    std::string cmd;                              // full shell command (if shell=true or argv empty)
    std::string cwd;                              // working directory (optional)
    std::map<std::string, std::string> env;       // extra environment (optional)
};

// --------- Orchestrator ---------
class EnhancedMessageInterceptor; // interface (defined in websocket_proxy.hpp)
class BeastWebSocketProxy;        // fwd

class UnifiedOrchestrator {
public:
    struct Config {
        // Connection
        std::string server_path = "ws://127.0.0.1:8765";

        // Legacy (kept for convenience; overridden by CommandSpec if provided)
        std::string browser_path = "/usr/bin/google-chrome";
        std::string browser_url  = "http://localhost:3000";
        std::string cpp_client_path = "./cpp_client";
        bool enable_proxy = true;
        uint16_t proxy_listen_port = 9797;
        // New: exact commands via config
        CommandSpec browser; // used in mode=browser+cpp
        CommandSpec cpp;     // used in both modes

        // Sampling
        unsigned gpu_index = 0;
        unsigned os_monitor_interval_ms  = 100;
        unsigned gpu_monitor_interval_ms = 50;

        // Run control
        int         duration_sec = 0; // 0 = run until signal
        std::string output_dir = "./metrics";
        bool        export_detailed_samples = true;
    };

    UnifiedOrchestrator();
    ~UnifiedOrchestrator();

    bool runBrowserPlusCpp(const Config& cfg);
    bool runCppOnly(const Config& cfg);
    void stop();

private:
    void setupMessageProxy(const std::string& upstreamUrl);
    void exportMetrics(const Config& cfg);

private:
    std::unique_ptr<OSMetricsCollector>  os_collector_;
    std::unique_ptr<GPUMetricsCollector> gpu_collector_;
    std::unique_ptr<ChunkTracker>        chunk_tracker_;

    EnhancedMessageInterceptor* interceptor_;
    std::atomic<bool> running_{false};
};

} // namespace unified_monitor
