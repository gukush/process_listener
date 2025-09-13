// simple_main.cpp - Simplified orchestrator with Apache ORC storage
// Removes WebSocket handling and chunk tracking, focuses on process spawning and metrics collection
//
// Example config (config.json):
// {
//   "mode": "browser+cpp",                        // or "cpp-only"
//   "server": "ws://127.0.0.1:8765",
//
//   "browser": {
//     "enabled": true,
//     "argv": ["/usr/bin/google-chrome", "--new-window", "http://localhost:3000"],
//     "cwd": "/",
//     "env": { "DISPLAY": ":0" },
//     "shell": false
//   },
//
//   "cpp_client": {
//     "enabled": true,
//     "argv": ["./cpp_client"],
//     "cwd": "/home/user/project",
//     "env": { },
//     "shell": false
//   },
//
//   "gpu_index": 0,
//   "os_interval_ms": 100,
//   "gpu_interval_ms": 50,
//   "duration_sec": 0,
//   "output_dir": "./metrics",
//   "storage": {
//     "max_rows_per_file": 100000,
//     "max_file_age_minutes": 5,
//     "use_zstd_compression": true,
//     "zstd_compression_level": 3
//   }
// }

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>     // fork, execvp, chdir
#include <stdlib.h>     // setenv

#include <nlohmann/json.hpp>

#if HAVE_CUDA
  #if __has_include(<nvml.h>)
    #include <nvml.h>
  #else
    #include <nvidia-ml/nvml.h>
  #endif
#endif

#include "simple_orchestrator.hpp"

using json = nlohmann::json;

namespace unified_monitor {

// --------------------------- signals -----------------------------------------
static std::atomic<bool> g_interrupted = false;
static void handle_signal(int) {
    g_interrupted = true;
    std::cout << "\n[Signal] Interrupt received, shutting down gracefully..." << std::endl;
}

// --------------------------- process scanning -----------------------------------
std::string SimpleOrchestrator::getProcessCmdline(pid_t pid) {
    std::string cmdline;
    std::ifstream cmdline_file("/proc/" + std::to_string(pid) + "/cmdline");
    if (cmdline_file.is_open()) {
        std::getline(cmdline_file, cmdline);
        // Replace null bytes with spaces for easier parsing
        std::replace(cmdline.begin(), cmdline.end(), '\0', ' ');
        // Remove trailing whitespace
        cmdline.erase(cmdline.find_last_not_of(" \t\n\r\f\v") + 1);
    }
    return cmdline;
}

bool SimpleOrchestrator::hasChromeDataDir(pid_t pid, const std::string& data_dir) {
    if (data_dir.empty()) {
        return true; // No filtering if data_dir not specified
    }

    std::string cmdline = getProcessCmdline(pid);
    if (cmdline.empty()) {
        return false;
    }

    // Look for --user-data-dir argument in the command line
    std::string search_pattern = "--user-data-dir=" + data_dir;
    return cmdline.find(search_pattern) != std::string::npos;
}

std::vector<pid_t> SimpleOrchestrator::getPidsByName(const std::string& process_name) {
    std::vector<pid_t> pids;

    // Read /proc directory to find processes
    std::filesystem::path proc_path("/proc");
    if (!std::filesystem::exists(proc_path)) {
        std::cerr << "[Scan] /proc directory not found" << std::endl;
        return pids;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(proc_path)) {
            if (!entry.is_directory()) continue;

            std::string dir_name = entry.path().filename().string();

            // Check if directory name is a number (PID)
            if (std::all_of(dir_name.begin(), dir_name.end(), ::isdigit)) {
                pid_t pid = static_cast<pid_t>(std::stoi(dir_name));

                // Read process name from /proc/PID/comm
                std::ifstream comm_file(entry.path() / "comm");
                if (comm_file.is_open()) {
                    std::string comm;
                    if (std::getline(comm_file, comm)) {
                        // Remove trailing newline if present
                        if (!comm.empty() && comm.back() == '\n') {
                            comm.pop_back();
                        }

                        if (comm == process_name) {
                            pids.push_back(pid);
                            std::cout << "[Scan] Found " << process_name << " with PID " << pid << std::endl;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Scan] Error scanning processes: " << e.what() << std::endl;
    }

    return pids;
}

std::vector<pid_t> SimpleOrchestrator::scanForProcesses(const std::vector<std::string>& process_names) {
    std::vector<pid_t> all_pids;

    for (const auto& name : process_names) {
        auto pids = getPidsByName(name);
        all_pids.insert(all_pids.end(), pids.begin(), pids.end());
    }

    if (all_pids.empty()) {
        std::cout << "[Scan] No target processes found. Make sure "
                  << "google-chrome and/or native_client are running." << std::endl;
    } else {
        std::cout << "[Scan] Found " << all_pids.size() << " target processes to monitor" << std::endl;
    }

    return all_pids;
}

std::vector<pid_t> SimpleOrchestrator::scanForProcesses(const std::vector<std::string>& process_names, const std::string& chrome_data_dir) {
    std::vector<pid_t> all_pids;

    for (const auto& name : process_names) {
        auto pids = getPidsByName(name);

        // Apply Chrome data directory filtering if specified
        if (name == "chrome" && !chrome_data_dir.empty()) {
            std::vector<pid_t> filtered_pids;
            for (pid_t pid : pids) {
                if (hasChromeDataDir(pid, chrome_data_dir)) {
                    filtered_pids.push_back(pid);
                    std::string cmdline = getProcessCmdline(pid);
                    std::cout << "[Scan] Chrome PID " << pid << " matches data dir filter: " << cmdline << std::endl;
                }
            }
            pids = filtered_pids;
        }

        all_pids.insert(all_pids.end(), pids.begin(), pids.end());
    }

    if (all_pids.empty()) {
        std::cout << "[Scan] No target processes found. Make sure "
                  << "google-chrome and/or native_client are running." << std::endl;
        if (!chrome_data_dir.empty()) {
            std::cout << "[Scan] Chrome data directory filter: " << chrome_data_dir << std::endl;
        }
    } else {
        std::cout << "[Scan] Found " << all_pids.size() << " target processes to monitor" << std::endl;
    }

    return all_pids;
}

// --------- OSMetricsCollector thread orchestration ----------
OSMetricsCollector::OSMetricsCollector() = default;
OSMetricsCollector::~OSMetricsCollector() { stopMonitoring(); }

void OSMetricsCollector::startMonitoring(const std::vector<pid_t>& pids, unsigned interval_ms) {
    stopMonitoring();
    monitored_pids_ = pids;
    interval_ms_ = interval_ms ? interval_ms : 200;
    running_ = true;
    monitor_thread_ = std::thread([this]() {
        while (running_) {
            auto now = Clock::now();
            std::vector<OSMetrics> batch;
            batch.reserve(monitored_pids_.size());
            for (pid_t pid : monitored_pids_) {
                if (pid <= 0) continue;
                try {
                    auto m = collectForPid(pid);
                    batch.push_back(m);
                } catch (...) {}
            }
            if (!batch.empty()) {
                std::lock_guard<std::mutex> lock(metrics_mutex_);
                metrics_.insert(metrics_.end(), batch.begin(), batch.end());
            }
            std::this_thread::sleep_until(now + std::chrono::milliseconds(interval_ms_));
        }
    });
}

void OSMetricsCollector::stopMonitoring() {
    if (!running_) return;
    running_ = false;
    if (monitor_thread_.joinable()) monitor_thread_.join();
}

std::vector<OSMetrics> OSMetricsCollector::getMetrics() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    return metrics_;
}

// ------------------------ GPUMetricsCollector impl ---------------------------
#if HAVE_CUDA
static std::string nvml_err_str(nvmlReturn_t st) {
    const char* s = nvmlErrorString(st);
    return s ? std::string(s) : "NVML_ERROR";
}
#endif

GPUMetricsCollector::GPUMetricsCollector(unsigned gpu_index) : gpu_index_(gpu_index) {}
GPUMetricsCollector::~GPUMetricsCollector() { stopMonitoring(); }

void GPUMetricsCollector::startMonitoring(unsigned interval_ms, const std::vector<pid_t>& monitored_pids) {
#if HAVE_CUDA
    stopMonitoring();
    interval_ms_ = interval_ms ? interval_ms : 100;
    monitored_pids_ = monitored_pids;
    running_ = true;
    worker_ = std::thread([this]() {
        nvmlReturn_t st = nvmlInit_v2();
        if (st != NVML_SUCCESS) { std::cerr << "[NVML] init failed: " << nvml_err_str(st) << "\n"; running_ = false; return; }
        nvmlDevice_t dev{};
        st = nvmlDeviceGetHandleByIndex_v2(gpu_index_, &dev);
        if (st != NVML_SUCCESS) { std::cerr << "[NVML] device " << gpu_index_ << " error: " << nvml_err_str(st) << "\n"; nvmlShutdown(); running_ = false; return; }

        while (running_) {
            auto t = Clock::now();
            GPUMetrics m{};
            m.timestamp = std::chrono::duration<double>(t.time_since_epoch()).count();
            m.gpu_index = gpu_index_;

            unsigned int power = 0;
            if (nvmlDeviceGetPowerUsage(dev, &power) == NVML_SUCCESS) m.power_mw = power;

            nvmlUtilization_t util{};
            if (nvmlDeviceGetUtilizationRates(dev, &util) == NVML_SUCCESS) {
                m.gpu_util_percent = util.gpu;
                m.mem_util_percent = util.memory;
            }

            nvmlMemory_t mem{};
            if (nvmlDeviceGetMemoryInfo(dev, &mem) == NVML_SUCCESS) m.mem_used_bytes = mem.used;

            unsigned int sm = 0;
            if (nvmlDeviceGetClockInfo(dev, NVML_CLOCK_SM, &sm) == NVML_SUCCESS) m.sm_clock_mhz = sm;

            unsigned int temp = 0;
            if (nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) m.temperature_c = temp;

            // Get per-process GPU utilization
            const unsigned int MAX_SAMPLES = 1024;
            std::vector<nvmlProcessUtilizationSample_t> samples(MAX_SAMPLES);
            unsigned int n = MAX_SAMPLES;
            st = nvmlDeviceGetProcessUtilization(dev, samples.data(), &n, 0);
            if (st == NVML_SUCCESS) {
                for (unsigned int i = 0; i < n; ++i) {
                    const auto& s = samples[i];
                    pid_t pid = static_cast<pid_t>(s.pid);

                    // Only track PIDs we're monitoring (if any specified)
                    if (monitored_pids_.empty() ||
                        std::find(monitored_pids_.begin(), monitored_pids_.end(), pid) != monitored_pids_.end()) {
                        m.pid_gpu_percent[static_cast<unsigned int>(s.pid)] = s.smUtil;
                    }
                }
            }

            { std::lock_guard<std::mutex> lk(mx_); samples_.push_back(std::move(m)); }

            std::this_thread::sleep_until(t + std::chrono::milliseconds(interval_ms_));
        }

        nvmlShutdown();
    });
#else
    (void)interval_ms;
    running_ = false; // disabled
#endif
}

void GPUMetricsCollector::stopMonitoring() {
    if (!running_) return;
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

std::vector<GPUMetrics> GPUMetricsCollector::getMetrics() const {
    std::lock_guard<std::mutex> lk(mx_);
    return samples_;
}

// ------------------------------ SimpleOrchestrator --------------------------
SimpleOrchestrator::SimpleOrchestrator()
    : os_collector_(std::make_unique<OSMetricsCollector>()),
      gpu_collector_(std::make_unique<GPUMetricsCollector>(0)),
      storage_(std::make_unique<MetricsStorage>()),
      websocket_listener_(std::make_unique<WebSocketListener>()) {}

SimpleOrchestrator::~SimpleOrchestrator() { stop(); }

void SimpleOrchestrator::setStorageConfig(const MetricsStorage::Config& config) {
    storage_->setStorageConfig(config);
}

bool SimpleOrchestrator::run(const Config& cfg) {
    running_ = true;

    // Configure storage with the provided config
    MetricsStorage::Config storage_config = cfg.storage_config;
    storage_config.output_dir = cfg.output_dir;  // Use the output_dir from the main config
    setStorageConfig(storage_config);

    // Initialize storage
    if (!storage_->initialize()) {
        std::cerr << "[Orchestrator] Failed to initialize storage" << std::endl;
        return false;
    }

    // Scan for target processes or use specific PID
    if (cfg.target_pid > 0) {
        // Monitor specific PID
        monitored_pids_ = {cfg.target_pid};
        std::cout << "[Orchestrator] Monitoring specific PID: " << cfg.target_pid << std::endl;

        // Verify the PID exists
        std::ifstream proc_file("/proc/" + std::to_string(cfg.target_pid) + "/stat");
        if (!proc_file.is_open()) {
            std::cerr << "[Orchestrator] PID " << cfg.target_pid << " does not exist or is not accessible" << std::endl;
            return false;
        }
        proc_file.close();
    } else {
        // Scan for target processes by name
        monitored_pids_ = scanForProcesses(cfg.target_process_names, cfg.chrome_data_dir);

        if (monitored_pids_.empty()) {
            std::cerr << "[Orchestrator] No target processes found to monitor" << std::endl;
            return false;
        }
    }

    // Set up websocket connection if enabled
    if (cfg.enable_websocket) {
        setupWebSocket(cfg);
    } else {
        // If websocket is disabled, start metrics collection immediately
        startMetricsCollection(cfg);
        metrics_collecting_ = true;
    }

    // Wait loop
    auto t0 = Clock::now();
    while (running_) {
        if (g_interrupted) break;
        if (cfg.duration_sec > 0 && Clock::now() - t0 > std::chrono::seconds(cfg.duration_sec)) break;

        // Periodically flush metrics to storage
        if (metrics_collecting_) {
            flushMetrics();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    // Stop collectors and export
    stopMetricsCollection();
    exportSummary(cfg);

    return true;
}

void SimpleOrchestrator::stop() {
    running_ = false;
    if (websocket_listener_) {
        websocket_listener_->disconnect();
    }
}

void SimpleOrchestrator::setupWebSocket(const Config& cfg) {
    // Set up websocket callbacks
    websocket_listener_->onStartMetrics = [this, &cfg]() {
        std::cout << "[Orchestrator] WebSocket: Starting metrics collection..." << std::endl;
        startMetricsCollection(cfg);
        metrics_collecting_ = true;
    };

    websocket_listener_->onStopMetrics = [this]() {
        std::cout << "[Orchestrator] WebSocket: Stopping metrics collection..." << std::endl;
        stopMetricsCollection();
        metrics_collecting_ = false;
    };

    // Connect to websocket server
    if (!websocket_listener_->connect(cfg.websocket_host, cfg.websocket_port, cfg.websocket_target, cfg.websocket_use_ssl)) {
        std::cerr << "[Orchestrator] Failed to connect to websocket server. Starting metrics collection immediately." << std::endl;
        startMetricsCollection(cfg);
        metrics_collecting_ = true;
    } else {
        std::cout << "[Orchestrator] Connected to websocket server. Waiting for start signal..." << std::endl;
    }
}

void SimpleOrchestrator::startMetricsCollection(const Config& cfg) {
    std::cout << "[Orchestrator] ===== MEASURING STARTS =====" << std::endl;

    // Start OS metrics collection with scanned PIDs
    os_collector_->startMonitoring(monitored_pids_, cfg.os_monitor_interval_ms);

    // Start GPU metrics collection with monitored PIDs for per-process filtering
    gpu_collector_.reset(new GPUMetricsCollector(cfg.gpu_index));
#if HAVE_CUDA
    gpu_collector_->startMonitoring(cfg.gpu_monitor_interval_ms, monitored_pids_);
#else
    std::cout << "[GPU] HAVE_CUDA=0 -> GPU/NVML metrics disabled." << std::endl;
#endif
}

void SimpleOrchestrator::stopMetricsCollection() {
    std::cout << "[Orchestrator] ===== MEASURING ENDS =====" << std::endl;

    os_collector_->stopMonitoring();
#if HAVE_CUDA
    gpu_collector_->stopMonitoring();
#endif
}

void SimpleOrchestrator::flushMetrics() {
    // Get and store OS metrics
    auto os_metrics = os_collector_->getMetrics();
    if (!os_metrics.empty()) {
        storage_->addOSMetrics(os_metrics);
    }

    // Get and store GPU metrics
#if HAVE_CUDA
    auto gpu_metrics = gpu_collector_->getMetrics();
    if (!gpu_metrics.empty()) {
        storage_->addGPUMetrics(gpu_metrics);
    }
#endif
}

void SimpleOrchestrator::exportSummary(const Config& config) {
    std::cout << "[Orchestrator] Finalizing metrics storage..." << std::endl;

    // Flush any remaining metrics first
    flushMetrics();

    // CRITICAL FIX: Use flushAndClose() instead of flush()
    // to ensure ORC files are properly closed with footer/metadata
    storage_->flushAndClose();

    // Print summary
    auto stats = storage_->getStats();
    std::cout << "\n[Metrics Summary]" << std::endl;
    std::cout << "OS samples collected: " << stats.total_os_samples << std::endl;
    std::cout << "GPU samples collected: " << stats.total_gpu_samples << std::endl;
    std::cout << "OS files written: " << stats.os_files_written << std::endl;
    std::cout << "GPU files written: " << stats.gpu_files_written << std::endl;
    if (!stats.last_os_file.empty()) {
        std::cout << "Last OS file: " << stats.last_os_file << std::endl;
    }
    if (!stats.last_gpu_file.empty()) {
        std::cout << "Last GPU file: " << stats.last_gpu_file << std::endl;
    }
    std::cout << "[Orchestrator] Shutdown complete." << std::endl;
}

// ----------------------------------- CLI & Config ----------------------------
static void print_usage(const char* argv0) {
    std::cerr <<
    "Usage:\n"
    "  " << argv0 << " [--gpu-index N] [--os-interval MS] [--gpu-interval MS]\n"
    "               [--duration SEC] [--out-dir DIR]\n"
    "               [--process-names NAME1,NAME2,...]\n"
    "               [--data-dir DIR] [--pid PID]\n"
    "               [--websocket] [--ws-host HOST] [--ws-port PORT]\n"
    "               [--ws-target PATH] [--ws-no-ssl]\n"
    "\n"
    "This tool scans for running processes named 'chrome' and 'native_client'\n"
    "by default and monitors their OS and GPU metrics.\n"
    "\n"
    "Options:\n"
    "  --data-dir DIR    Filter Chrome processes by --user-data-dir argument\n"
    "                    Only monitor Chrome processes that use this data directory\n"
    "  --pid PID         Monitor a specific process by its PID instead of scanning\n"
    "                    for process names. Takes precedence over --process-names\n"
    "  --websocket       Enable WebSocket connection for remote metrics control\n"
    "  --ws-host HOST    WebSocket server host (default: 127.0.0.1)\n"
    "  --ws-port PORT    WebSocket server port (default: 8765)\n"
    "  --ws-target PATH  WebSocket target path (default: /ws-listener)\n"
    "  --ws-no-ssl       Disable SSL for WebSocket connection\n";
}

// Helper function to split comma-separated string
static std::vector<std::string> split_string(const std::string& str, char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delimiter)) {
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

} // namespace unified_monitor

int main(int argc, char** argv) {
    using namespace unified_monitor;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    SimpleOrchestrator::Config cfg;

    // -------------------------- CLI parse --------------------------
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) {
            if (i + 1 >= argc) { std::cerr << name << " requires value\n"; print_usage(argv[0]); std::exit(2); }
            return std::string(argv[++i]);
        };

        if (a == "--gpu-index") {
            cfg.gpu_index = static_cast<unsigned>(std::stoul(need("--gpu-index")));
        }
        else if (a == "--os-interval") {
            cfg.os_monitor_interval_ms = static_cast<unsigned>(std::stoul(need("--os-interval")));
        }
        else if (a == "--gpu-interval") {
            cfg.gpu_monitor_interval_ms = static_cast<unsigned>(std::stoul(need("--gpu-interval")));
        }
        else if (a == "--duration") {
            cfg.duration_sec = std::stoi(need("--duration"));
        }
        else if (a == "--out-dir") {
            cfg.output_dir = need("--out-dir");
        }
        else if (a == "--process-names") {
            cfg.target_process_names = split_string(need("--process-names"), ',');
        }
        else if (a == "--data-dir") {
            cfg.chrome_data_dir = need("--data-dir");
        }
        else if (a == "--pid") {
            cfg.target_pid = static_cast<pid_t>(std::stoi(need("--pid")));
        }
        else if (a == "--websocket") {
            cfg.enable_websocket = true;
        }
        else if (a == "--ws-host") {
            cfg.websocket_host = need("--ws-host");
        }
        else if (a == "--ws-port") {
            cfg.websocket_port = need("--ws-port");
        }
        else if (a == "--ws-target") {
            cfg.websocket_target = need("--ws-target");
        }
        else if (a == "--ws-no-ssl") {
            cfg.websocket_use_ssl = false;
        }
        else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        else {
            std::cerr << "Unknown arg: " << a << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    SimpleOrchestrator orch;
    bool ok = orch.run(cfg);
    orch.stop();
    return ok ? 0 : 1;
}