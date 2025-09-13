#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "metrics_storage.hpp"
#include "websocket_listener.hpp"

namespace unified_monitor {

using Clock = std::chrono::steady_clock;

// ------------------------------ OSMetrics --------------------------
struct OSMetrics {
    double timestamp = 0.0;
    pid_t pid = 0;
    long mem_rss_kb = 0;        // Resident Set Size in KB
    long mem_vms_kb = 0;        // Virtual Memory Size in KB
    double cpu_percent = 0.0;   // CPU utilization percentage
    uint64_t disk_read_bytes = 0;
    uint64_t disk_write_bytes = 0;
    uint64_t net_recv_bytes = 0;
    uint64_t net_sent_bytes = 0;
};

// ------------------------------ OSMetricsCollector --------------------------
class OSMetricsCollector {
public:
    OSMetricsCollector();
    ~OSMetricsCollector();

    void startMonitoring(const std::vector<pid_t>& pids, unsigned interval_ms);
    void stopMonitoring();
    std::vector<OSMetrics> getMetrics() const;

    // Static method to collect metrics for a single PID
    static OSMetrics collectForPid(pid_t pid);

private:
    std::vector<pid_t> monitored_pids_;
    unsigned interval_ms_ = 200;
    std::atomic<bool> running_{false};
    std::thread monitor_thread_;
    mutable std::mutex metrics_mutex_;
    std::vector<OSMetrics> metrics_;
};

// ------------------------------ GPUMetrics --------------------------
struct GPUMetrics {
    double timestamp = 0.0;
    unsigned int gpu_index = 0;
    unsigned int power_mw = 0;
    unsigned int gpu_util_percent = 0;
    unsigned int mem_util_percent = 0;
    uint64_t mem_used_bytes = 0;
    unsigned int sm_clock_mhz = 0;
    unsigned int temperature_c = 0;
    std::map<unsigned int, unsigned int> pid_gpu_percent; // pid -> sm utilization %
};

// ------------------------------ GPUMetricsCollector --------------------------
class GPUMetricsCollector {
public:
    explicit GPUMetricsCollector(unsigned gpu_index);
    ~GPUMetricsCollector();

    void startMonitoring(unsigned interval_ms, const std::vector<pid_t>& monitored_pids = {});
    void stopMonitoring();
    std::vector<GPUMetrics> getMetrics() const;

private:
    unsigned gpu_index_;
    unsigned interval_ms_ = 100;
    std::vector<pid_t> monitored_pids_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex mx_;
    std::vector<GPUMetrics> samples_;
};

// ------------------------------ SimpleOrchestrator --------------------------
class SimpleOrchestrator {
public:
    struct Config {
        // Process monitoring
        std::vector<std::string> target_process_names = {"chrome", "native_client"};
        std::string chrome_data_dir;
        pid_t target_pid = 0; // If > 0, monitor specific PID instead of scanning

        // Metrics intervals
        unsigned gpu_index = 0;
        unsigned os_monitor_interval_ms = 200;
        unsigned gpu_monitor_interval_ms = 100;
        int duration_sec = 0; // 0 = run until interrupted

        // Output
        std::string output_dir = "./metrics";
        MetricsStorage::Config storage_config;

        // WebSocket connection (URL-based like main.cpp)
        std::string websocket_url; // e.g., "wss://127.0.0.1:3001" or "ws://127.0.0.1:3001"
        // Note: insecure connections (self-signed certs) are handled automatically in WebSocketListener
    };

    SimpleOrchestrator();
    ~SimpleOrchestrator();

    bool run(const Config& cfg);
    void stop();
    void setStorageConfig(const MetricsStorage::Config& config);

    // Process scanning utilities
    static std::vector<pid_t> getPidsByName(const std::string& process_name);
    static std::vector<pid_t> scanForProcesses(const std::vector<std::string>& process_names);
    static std::vector<pid_t> scanForProcesses(const std::vector<std::string>& process_names, const std::string& chrome_data_dir);
    static std::string getProcessCmdline(pid_t pid);
    static bool hasChromeDataDir(pid_t pid, const std::string& data_dir);

private:
    void setupWebSocket(const Config& cfg);
    void startMetricsCollection(const Config& cfg);
    void stopMetricsCollection();
    void flushMetrics();
    void exportSummary(const Config& config);

    std::unique_ptr<OSMetricsCollector> os_collector_;
    std::unique_ptr<GPUMetricsCollector> gpu_collector_;
    std::unique_ptr<MetricsStorage> storage_;
    std::unique_ptr<WebSocketListener> websocket_listener_;

    std::atomic<bool> running_{false};
    std::atomic<bool> metrics_collecting_{false};
    std::vector<pid_t> monitored_pids_;
};

} // namespace unified_monitor