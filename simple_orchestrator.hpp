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
#include <nlohmann/json.hpp>

#include "metrics_storage.hpp"

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
    unsigned   temperature_c = 0;  // GPU temperature in Celsius

    // Optional per-PID GPU util (smUtil) %
    std::map<unsigned, int> pid_gpu_percent;
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
    unsigned interval_ms_ = 200;
    std::atomic<bool> running_{false};
    mutable std::mutex metrics_mutex_;
    std::thread monitor_thread_;
    std::vector<OSMetrics> metrics_;
};

class GPUMetricsCollector {
public:
    explicit GPUMetricsCollector(unsigned gpu_index);
    ~GPUMetricsCollector();

    void startMonitoring(unsigned interval_ms, const std::vector<pid_t>& monitored_pids = {});
    void stopMonitoring();
    std::vector<GPUMetrics> getMetrics() const;

private:
    unsigned gpu_index_ = 0;
    unsigned interval_ms_ = 100;
    std::vector<pid_t> monitored_pids_;
    std::atomic<bool> running_{false};
    mutable std::mutex mx_;
    std::thread worker_;
    std::vector<GPUMetrics> samples_;
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

// --------- Simplified Orchestrator ---------

class SimpleOrchestrator {
public:
    struct Config {
        // Process names to scan for (no more spawning)
        std::vector<std::string> target_process_names = {"chrome", "native_client"};

        // Chrome data directory filtering
        std::string chrome_data_dir = "";  // If specified, only monitor Chrome processes with this --user-data-dir

        // Sampling
        unsigned gpu_index = 0;
        unsigned os_monitor_interval_ms  = 200;  // 200ms for OS metrics
        unsigned gpu_monitor_interval_ms = 100;   // 100ms for GPU metrics

        // Run control
        int         duration_sec = 0; // 0 = run until signal
        std::string output_dir = "./metrics";

        // Storage configuration
        MetricsStorage::Config storage_config;
    };

    SimpleOrchestrator();
    ~SimpleOrchestrator();

    bool run(const Config& cfg);
    void stop();

    // Update storage configuration (must be called before run())
    void setStorageConfig(const MetricsStorage::Config& config);

private:
    // Process scanning
    std::vector<pid_t> scanForProcesses(const std::vector<std::string>& process_names);
    std::vector<pid_t> scanForProcesses(const std::vector<std::string>& process_names, const std::string& chrome_data_dir);
    std::vector<pid_t> getPidsByName(const std::string& process_name);
    std::string getProcessCmdline(pid_t pid);
    bool hasChromeDataDir(pid_t pid, const std::string& data_dir);

    // Metrics collection - Fix: change signature to match implementation
    void startMetricsCollection(const Config& cfg);
    void stopMetricsCollection();
    void flushMetrics();

    // Export functions
    void exportSummary(const Config& config);

private:
    std::unique_ptr<OSMetricsCollector>  os_collector_;
    std::unique_ptr<GPUMetricsCollector> gpu_collector_;
    std::unique_ptr<MetricsStorage>      storage_;

    std::atomic<bool> running_{false};
    std::vector<pid_t> monitored_pids_;
};

} // namespace unified_monitor
