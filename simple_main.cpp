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
#include "os_metrics_linux.cpp"

using json = nlohmann::json;

namespace unified_monitor {

// --------------------------- signals -----------------------------------------
static std::atomic<bool> g_interrupted = false;
static void handle_signal(int) { g_interrupted = true; }

// --------------------------- process spawn -----------------------------------
SimpleOrchestrator::ChildProcess SimpleOrchestrator::spawnProcess(const CommandSpec& spec, const std::string& name) {
    if (!spec.enabled) return {-1, name};

    // Build cargv for execvp OR for /bin/sh -lc
    std::vector<std::string> local_argv;
    if (spec.shell) {
        // Join argv as a single string command
        std::string joined;
        for (size_t i = 0; i < spec.argv.size(); ++i) {
            if (i) joined += ' ';
            joined += spec.argv[i];
        }
        local_argv = { "/bin/sh", "-lc", joined };
    } else {
        local_argv = spec.argv;
        if (local_argv.empty()) {
            std::cerr << "[Spawn] Empty argv for " << name << "; nothing to exec\n";
            return {-1, name};
        }
    }

    std::vector<char*> cargv;
    cargv.reserve(local_argv.size() + 1);
    for (auto& s : local_argv) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return {-1, name};
    }
    if (pid == 0) {
        // Child: set CWD and ENV if requested
        if (!spec.cwd.empty()) {
            if (chdir(spec.cwd.c_str()) != 0) {
                perror("chdir");
                _exit(127);
            }
        }
        for (const auto& kv : spec.env) {
            ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
        }
        // Exec
        execvp(cargv[0], cargv.data());
        perror("execvp");
        _exit(127);
    }
    return {pid, name};
}

void SimpleOrchestrator::terminateProcess(ChildProcess& proc) {
    if (proc.pid > 0) {
        std::cout << "[Spawn] Terminating " << proc.name << " (pid=" << proc.pid << ")" << std::endl;
        kill(proc.pid, SIGTERM);
        proc.pid = -1;
    }
}

// --------- OSMetricsCollector thread orchestration ----------
OSMetricsCollector::OSMetricsCollector() = default;
OSMetricsCollector::~OSMetricsCollector() { stopMonitoring(); }

void OSMetricsCollector::startMonitoring(const std::vector<pid_t>& pids, unsigned interval_ms) {
    stopMonitoring();
    monitored_pids_ = pids;
    interval_ms_ = interval_ms ? interval_ms : 100;
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

void GPUMetricsCollector::startMonitoring(unsigned interval_ms) {
#if HAVE_CUDA
    stopMonitoring();
    interval_ms_ = interval_ms ? interval_ms : 50;
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

            const unsigned int MAX_SAMPLES = 1024;
            std::vector<nvmlProcessUtilizationSample_t> samples(MAX_SAMPLES);
            unsigned int n = MAX_SAMPLES;
            st = nvmlDeviceGetProcessUtilization(dev, samples.data(), &n, 0);
            if (st == NVML_SUCCESS) {
                for (unsigned int i = 0; i < n; ++i) {
                    const auto& s = samples[i];
                    m.pid_gpu_percent[static_cast<unsigned int>(s.pid)] = s.smUtil;
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
      storage_(std::make_unique<MetricsStorage>()) {}

SimpleOrchestrator::~SimpleOrchestrator() { stop(); }

bool SimpleOrchestrator::runBrowserPlusCpp(const Config& cfg) {
    running_ = true;

    // Initialize storage
    if (!storage_->initialize()) {
        std::cerr << "[Orchestrator] Failed to initialize storage" << std::endl;
        return false;
    }

    // Spawn processes
    spawned_processes_.clear();

    if (cfg.browser.enabled) {
        auto browser = spawnProcess(cfg.browser, "browser");
        if (browser.pid > 0) {
            spawned_processes_.push_back(browser);
            std::cout << "[Spawn] Browser pid=" << browser.pid << std::endl;
        } else {
            std::cerr << "[Spawn] Browser failed to start" << std::endl;
        }
    }

    if (cfg.cpp.enabled) {
        auto cpp = spawnProcess(cfg.cpp, "cpp_client");
        if (cpp.pid > 0) {
            spawned_processes_.push_back(cpp);
            std::cout << "[Spawn] C++ client pid=" << cpp.pid << std::endl;
        } else {
            std::cerr << "[Spawn] C++ client failed to start" << std::endl;
        }
    }

    if (spawned_processes_.empty()) {
        std::cerr << "[Orchestrator] No processes spawned successfully" << std::endl;
        return false;
    }

    // Start metrics collection
    startMetricsCollection(cfg);

    // Wait loop
    auto t0 = Clock::now();
    while (running_) {
        if (g_interrupted) break;
        if (cfg.duration_sec > 0 && Clock::now() - t0 > std::chrono::seconds(cfg.duration_sec)) break;

        // Periodically flush metrics to storage
        flushMetrics();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    // Stop collectors and export
    stopMetricsCollection();
    exportSummary(cfg);

    // Terminate processes
    for (auto& proc : spawned_processes_) {
        terminateProcess(proc);
    }

    return true;
}

bool SimpleOrchestrator::runCppOnly(const Config& cfg) {
    running_ = true;

    // Initialize storage
    if (!storage_->initialize()) {
        std::cerr << "[Orchestrator] Failed to initialize storage" << std::endl;
        return false;
    }

    // Spawn C++ client only
    spawned_processes_.clear();

    if (cfg.cpp.enabled) {
        auto cpp = spawnProcess(cfg.cpp, "cpp_client");
        if (cpp.pid > 0) {
            spawned_processes_.push_back(cpp);
            std::cout << "[Spawn] C++ client pid=" << cpp.pid << std::endl;
        } else {
            std::cerr << "[Spawn] C++ client failed to start" << std::endl;
            return false;
        }
    }

    // Start metrics collection
    startMetricsCollection(cfg);

    // Wait loop
    auto t0 = Clock::now();
    while (running_) {
        if (g_interrupted) break;
        if (cfg.duration_sec > 0 && Clock::now() - t0 > std::chrono::seconds(cfg.duration_sec)) break;

        // Periodically flush metrics to storage
        flushMetrics();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    // Stop collectors and export
    stopMetricsCollection();
    exportSummary(cfg);

    // Terminate processes
    for (auto& proc : spawned_processes_) {
        terminateProcess(proc);
    }

    return true;
}

void SimpleOrchestrator::stop() {
    running_ = false;
}

void SimpleOrchestrator::startMetricsCollection(const Config& cfg) {
    // Collect PIDs for OS monitoring
    std::vector<pid_t> pids;
    for (const auto& proc : spawned_processes_) {
        if (proc.pid > 0) pids.push_back(proc.pid);
    }

    // Start OS metrics collection
    os_collector_->startMonitoring(pids, cfg.os_monitor_interval_ms);

    // Start GPU metrics collection
    gpu_collector_.reset(new GPUMetricsCollector(cfg.gpu_index));
#if HAVE_CUDA
    gpu_collector_->startMonitoring(cfg.gpu_monitor_interval_ms);
#else
    std::cout << "[GPU] HAVE_CUDA=0 -> GPU/NVML metrics disabled." << std::endl;
#endif
}

void SimpleOrchestrator::stopMetricsCollection() {
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
    // Flush any remaining metrics
    flushMetrics();
    storage_->flush();

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
}

// ----------------------------------- CLI & Config ----------------------------
static void print_usage(const char* argv0) {
    std::cerr <<
    "Usage:\n"
    "  " << argv0 << " [--config FILE.json]\n"
    "               [--mode browser+cpp|cpp-only]\n"
    "               [--server ws://HOST:PORT]\n"
    "               [--browser-path PATH] [--browser-url URL]\n"
    "               [--cpp-client PATH]\n"
    "               [--gpu-index N] [--os-interval MS] [--gpu-interval MS]\n"
    "               [--duration SEC] [--out-dir DIR]\n";
}

static bool load_config(const std::string& path, SimpleOrchestrator::Config& cfg_out) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "[Config] Cannot open: " << path << "\n";
        return false;
    }
    json j; f >> j;

    if (j.contains("server")) cfg_out.server_path = j["server"].get<std::string>();
    if (j.contains("gpu_index")) cfg_out.gpu_index = j["gpu_index"].get<unsigned>();
    if (j.contains("os_interval_ms"))  cfg_out.os_monitor_interval_ms  = j["os_interval_ms"].get<unsigned>();
    if (j.contains("gpu_interval_ms")) cfg_out.gpu_monitor_interval_ms = j["gpu_interval_ms"].get<unsigned>();
    if (j.contains("duration_sec")) cfg_out.duration_sec = j["duration_sec"].get<int>();
    if (j.contains("output_dir")) cfg_out.output_dir = j["output_dir"].get<std::string>();

    auto parse_proc = [](const json& jp) -> CommandSpec {
        CommandSpec sp;
        if (jp.contains("enabled")) sp.enabled = jp["enabled"].get<bool>();
        if (jp.contains("cwd")) sp.cwd = jp["cwd"].get<std::string>();
        if (jp.contains("shell")) sp.shell = jp["shell"].get<bool>();
        if (jp.contains("argv") && jp["argv"].is_array()) {
            for (const auto& a : jp["argv"]) sp.argv.push_back(a.get<std::string>());
        }
        if (jp.contains("env") && jp["env"].is_object()) {
            for (auto it = jp["env"].begin(); it != jp["env"].end(); ++it) {
                sp.env[it.key()] = it.value().get<std::string>();
            }
        }
        return sp;
    };

    if (j.contains("browser") && j["browser"].is_object()) {
        cfg_out.browser = parse_proc(j["browser"]);
    }
    if (j.contains("cpp_client") && j["cpp_client"].is_object()) {
        cfg_out.cpp = parse_proc(j["cpp_client"]);
    }

    // Storage configuration
    if (j.contains("storage") && j["storage"].is_object()) {
        const auto& storage = j["storage"];
        if (storage.contains("max_rows_per_file")) {
            cfg_out.storage_config.max_rows_per_file = storage["max_rows_per_file"].get<size_t>();
        }
        if (storage.contains("max_file_age_minutes")) {
            cfg_out.storage_config.max_file_age = std::chrono::minutes(storage["max_file_age_minutes"].get<int>());
        }
        if (storage.contains("use_zstd_compression")) {
            cfg_out.storage_config.use_zstd_compression = storage["use_zstd_compression"].get<bool>();
        }
        if (storage.contains("zstd_compression_level")) {
            cfg_out.storage_config.zstd_compression_level = storage["zstd_compression_level"].get<int>();
        }
    }

    return true;
}

} // namespace unified_monitor

int main(int argc, char** argv) {
    using namespace unified_monitor;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    SimpleOrchestrator::Config cfg;
    std::string mode = "browser+cpp";
    cfg.server_path = "ws://127.0.0.1:8765";

    std::optional<std::string> config_path;

    // -------------------------- CLI parse --------------------------
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) {
            if (i + 1 >= argc) { std::cerr << name << " requires value\n"; print_usage(argv[0]); std::exit(2); }
            return std::string(argv[++i]);
        };
        if (a == "--config") config_path = need("--config");
        else if (a == "--mode") mode = need("--mode");
        else if (a == "--server") cfg.server_path = need("--server");
        else if (a == "--browser-path") {
            cfg.browser.argv = {need("--browser-path")};
            cfg.browser.enabled = true;
        }
        else if (a == "--browser-url") {
            if (cfg.browser.argv.empty()) cfg.browser.argv = {"/usr/bin/google-chrome"};
            cfg.browser.argv.push_back("--new-window");
            cfg.browser.argv.push_back(need("--browser-url"));
            cfg.browser.enabled = true;
        }
        else if (a == "--cpp-client") {
            cfg.cpp.argv = {need("--cpp-client")};
            cfg.cpp.enabled = true;
        }
        else if (a == "--gpu-index") cfg.gpu_index = static_cast<unsigned>(std::stoul(need("--gpu-index")));
        else if (a == "--os-interval") cfg.os_monitor_interval_ms = static_cast<unsigned>(std::stoul(need("--os-interval")));
        else if (a == "--gpu-interval") cfg.gpu_monitor_interval_ms = static_cast<unsigned>(std::stoul(need("--gpu-interval")));
        else if (a == "--duration") cfg.duration_sec = std::stoi(need("--duration"));
        else if (a == "--out-dir") cfg.output_dir = need("--out-dir");
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else { std::cerr << "Unknown arg: " << a << "\n"; print_usage(argv[0]); return 2; }
    }

    // -------------------------- Load config (optional) --------------------------
    if (config_path) {
        if (!load_config(*config_path, cfg)) {
            return 2;
        }
        // If config specifies mode, override CLI/default
        std::ifstream cf(*config_path);
        if (cf.good()) {
            json j; cf >> j;
            if (j.contains("mode") && j["mode"].is_string()) {
                mode = j["mode"].get<std::string>();
            }
        }
    }

    SimpleOrchestrator orch;

    bool ok = false;
    if (mode == "browser+cpp") ok = orch.runBrowserPlusCpp(cfg);
    else if (mode == "cpp-only") ok = orch.runCppOnly(cfg);
    else { std::cerr << "Invalid --mode\n"; return 2; }

    orch.stop();
    return ok ? 0 : 1;
}

