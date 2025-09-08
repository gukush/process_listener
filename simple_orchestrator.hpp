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
        // Connection
        std::string server_path = "ws://127.0.0.1:8765";

        // Process specifications
        CommandSpec browser; // used in mode=browser+cpp
        CommandSpec cpp;     // used in both modes

        // Sampling
        unsigned gpu_index = 0;
        unsigned os_monitor_interval_ms  = 100;   // 100ms for OS metrics
        unsigned gpu_monitor_interval_ms = 50;    // 50ms for GPU metrics

        // Run control
        int         duration_sec = 0; // 0 = run until signal
        std::string output_dir = "./metrics";

        // Storage configuration
        MetricsStorage::Config storage_config;
    };

    SimpleOrchestrator();
    ~SimpleOrchestrator();

    bool runBrowserPlusCpp(const Config& cfg);
    bool runCppOnly(const Config& cfg);
    void stop();

private:
    // Process management
    struct ChildProcess {
        pid_t pid = -1;
        std::string name;
    };

    ChildProcess spawnProcess(const CommandSpec& spec, const std::string& name);
    void terminateProcess(ChildProcess& proc);

    // Metrics collection
    void startMetricsCollection(const std::vector<pid_t>& pids, const Config& cfg);
    void stopMetricsCollection();
    void flushMetrics();

    // Export functions
    void exportSummary(const Config& config);

private:
    std::unique_ptr<OSMetricsCollector>  os_collector_;
    std::unique_ptr<GPUMetricsCollector> gpu_collector_;
    std::unique_ptr<MetricsStorage>      storage_;

    std::atomic<bool> running_{false};
    std::vector<ChildProcess> spawned_processes_;
};

} // namespace unified_monitor

