// orchestrator.hpp
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <fstream>

#include <nlohmann/json.hpp>
#include <nvml.h>

namespace unified_monitor {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// Data Structures

struct OSMetrics {
    double timestamp;
    unsigned int pid;
    double cpu_percent;
    unsigned long mem_rss_kb;
    unsigned long mem_vms_kb;
    unsigned long disk_read_bytes;
    unsigned long disk_write_bytes;
    unsigned long net_recv_bytes;
    unsigned long net_sent_bytes;
};

struct GPUMetrics {
    double timestamp;
    unsigned int gpu_index;
    unsigned int power_mw;
    unsigned int gpu_util_percent;
    unsigned int mem_util_percent;
    unsigned long long mem_used_bytes;
    unsigned int sm_clock_mhz;
    std::map<unsigned int, unsigned int> pid_gpu_percent;
};

struct ChunkMetrics {
    std::string chunk_id;
    std::string task_id;
    Clock::time_point arrival_time;
    Clock::time_point start_time;
    Clock::time_point completion_time;
    std::vector<OSMetrics> os_samples;
    std::vector<GPUMetrics> gpu_samples;
    double energy_joules;
    std::string status; // "pending", "processing", "completed", "failed"
};

// OS Metrics Collector

class OSMetricsCollector {
public:
    OSMetricsCollector();
    ~OSMetricsCollector();

    void startMonitoring(const std::vector<pid_t>& pids, unsigned interval_ms = 100);
    void stopMonitoring();
    std::vector<OSMetrics> getMetrics() const;

private:
    void monitorLoop();
    OSMetrics collectForPid(pid_t pid);

    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
    std::vector<pid_t> monitored_pids_;
    std::vector<OSMetrics> metrics_;
    mutable std::mutex metrics_mutex_;
    unsigned interval_ms_{100};

    // Per-process previous values for delta calculations
    struct ProcessState {
        unsigned long prev_utime{0};
        unsigned long prev_stime{0};
        unsigned long prev_disk_read{0};
        unsigned long prev_disk_write{0};
        unsigned long prev_net_recv{0};
        unsigned long prev_net_sent{0};
        Clock::time_point last_update;
    };
    std::map<pid_t, ProcessState> process_states_;
};

// GPU Metrics Collector (Enhanced)

class GPUMetricsCollector {
public:
    GPUMetricsCollector(unsigned gpu_index = 0);
    ~GPUMetricsCollector();

    void startMonitoring(unsigned interval_ms = 50);
    void stopMonitoring();
    std::vector<GPUMetrics> getMetrics() const;
    double getTotalEnergyJoules() const;

private:
    void monitorLoop();
    GPUMetrics collectMetrics();

    nvmlDevice_t device_handle_;
    unsigned gpu_index_;
    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
    std::vector<GPUMetrics> metrics_;
    mutable std::mutex metrics_mutex_;
    unsigned interval_ms_{50};

    // Energy integration
    double total_energy_joules_{0};
    unsigned int last_power_mw_{0};
    Clock::time_point last_sample_time_;
};

// Chunk Tracker

class ChunkTracker {
public:
    void onChunkArrival(const std::string& chunk_id, const std::string& task_id);
    void onChunkStart(const std::string& chunk_id);
    void onChunkComplete(const std::string& chunk_id, const std::string& status);

    void attachOSMetrics(const std::string& chunk_id, const std::vector<OSMetrics>& metrics);
    void attachGPUMetrics(const std::string& chunk_id, const std::vector<GPUMetrics>& metrics);

    ChunkMetrics getChunkMetrics(const std::string& chunk_id) const;
    std::vector<ChunkMetrics> getAllChunks() const;

    void exportToCSV(const std::string& filename) const;

private:
    mutable std::mutex chunks_mutex_;
    std::map<std::string, ChunkMetrics> chunks_;
};

// WebSocket Message Interceptor

class MessageInterceptor {
public:
    MessageInterceptor(ChunkTracker* tracker);

    // For intercepting socket.io messages to browser
    void interceptBrowserMessage(const json& message);

    // For intercepting WebSocket messages to C++ client
    void interceptNativeMessage(const json& message);

private:
    ChunkTracker* chunk_tracker_;

    bool isTaskInit(const json& msg) const;
    bool isChunkAssign(const json& msg) const;
    bool isChunkResult(const json& msg) const;

    std::string extractChunkId(const json& msg) const;
    std::string extractTaskId(const json& msg) const;
};

// Unified Orchestrator

class UnifiedOrchestrator {
public:
    UnifiedOrchestrator();
    ~UnifiedOrchestrator();

    struct Config {
        // Process paths
        std::string browser_path = "/usr/bin/google-chrome";
        std::string browser_url = "http://localhost:3000";
        std::string cpp_client_path = "./cpp_client";
        std::string server_path = "node server.js";

        // Monitoring settings
        unsigned gpu_index = 0;
        unsigned os_monitor_interval_ms = 100;
        unsigned gpu_monitor_interval_ms = 50;

        // Experiment settings
        int duration_sec = 60;

        // Output settings
        std::string output_dir = "./metrics";
        bool export_detailed_samples = true;
    };

    bool runBrowserPlusCpp(const Config& config);
    bool runCppOnly(const Config& config);

    void stop();

private:
    // Process management
    std::unique_ptr<ProcessManager> process_manager_;

    // Metrics collectors
    std::unique_ptr<OSMetricsCollector> os_collector_;
    std::unique_ptr<GPUMetricsCollector> gpu_collector_;

    // Chunk tracking
    std::unique_ptr<ChunkTracker> chunk_tracker_;
    std::unique_ptr<MessageInterceptor> interceptor_;

    // WebSocket proxy for message interception
    void setupMessageProxy(const std::string& target_url);

    // Export functions
    void exportMetrics(const Config& config);
    void exportChunkSummary(const std::string& filename);
    void exportDetailedSamples(const std::string& filename);

    std::atomic<bool> running_{false};
};

} // namespace unified_monitor