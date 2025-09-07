// main.cpp - Unified orchestrator with optional CUDA/NVML via HAVE_CUDA
// Adds JSON config support to launch exact commands for browser / C++ client.
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
//     "env": { "DISPLAY": ":0" },                 // e.g. required on X11
//     "shell": false                              // if true, run via /bin/sh -lc "<joined argv>"
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
//   "duration_sec": 0,                            // 0 = until signal
//   "output_dir": "./metrics",
//   "export_detailed_samples": true
// }
//
// Run:
//   ./unified_monitor --config ./config.json
//
// CLI still works without config:
//   ./unified_monitor --mode browser+cpp --server ws://127.0.0.1:8765 \
//       --browser-path /usr/bin/google-chrome --browser-url http://localhost:3000 \
//       --cpp-client ./cpp_client --gpu-index 0 --out-dir ./metrics

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

#include "orchestrator.hpp"
#include "gpu_listener.hpp"

using json = nlohmann::json;

namespace unified_monitor {

// ---------------- Global process-launch overrides from config -----------------
struct SpawnSpec {
    std::vector<std::string> argv;         // full argv (argv[0] is the executable)
    std::string cwd;                       // optional working directory
    std::map<std::string,std::string> env; // optional environment vars to set/override
    bool shell = false;                    // if true, run via /bin/sh -lc "<joined argv>"
    bool enabled = true;
};

static std::optional<SpawnSpec> g_browser_spec;
static std::optional<SpawnSpec> g_cpp_spec;

// --------------------------- signals -----------------------------------------
static std::atomic<bool> g_interrupted = false;
static void handle_signal(int) { g_interrupted = true; }

// --------------------------- process spawn -----------------------------------
struct ChildProcess { pid_t pid = -1; };

static ChildProcess spawn_process(const SpawnSpec& spec) {
    if (!spec.enabled) return {-1};

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
            std::cerr << "[Spawn] Empty argv; nothing to exec\n";
            return {-1};
        }
    }

    std::vector<char*> cargv;
    cargv.reserve(local_argv.size() + 1);
    for (auto& s : local_argv) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return {-1};
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
    return {pid};
}

// ---------------------- Direct Message Handler ------------------------
class DirectMessageHandler {
public:
    explicit DirectMessageHandler(ChunkTracker* tracker) : tracker_(tracker) {}

    void handleChunkArrival(const std::string& chunkId, const std::string& taskId) {
        tracker_->onChunkArrival(chunkId, taskId);
        tracker_->onChunkStart(chunkId);
    }

    void handleChunkComplete(const std::string& chunkId, const std::string& status) {
        tracker_->onChunkComplete(chunkId, status);
    }

private:
    ChunkTracker* tracker_;
};

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

// ------------------------------ UnifiedOrchestrator --------------------------
UnifiedOrchestrator::UnifiedOrchestrator()
    : os_collector_(std::make_unique<OSMetricsCollector>()),
      gpu_collector_(std::make_unique<GPUMetricsCollector>(0)),
      chunk_tracker_(std::make_unique<ChunkTracker>()) {}

UnifiedOrchestrator::~UnifiedOrchestrator() { stop(); }

// Enhanced URL parsing that preserves protocol information
struct ParsedUrl {
    std::string protocol;  // "ws", "wss", "http", "https"
    std::string host;
    uint16_t port;
    bool use_ssl;
};

static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl result;
    result.host = "127.0.0.1";
    result.port = 8765;
    result.use_ssl = false;
    result.protocol = "ws";

    try {
        std::string u = url;
        auto pos_protocol = u.find("://");
        if (pos_protocol != std::string::npos) {
            result.protocol = u.substr(0, pos_protocol);
            u = u.substr(pos_protocol + 3);

            // Determine SSL usage
            result.use_ssl = (result.protocol == "wss" || result.protocol == "https");
        }

        auto colon = u.find(':');
        auto slash = u.find('/');
        if (colon != std::string::npos) {
            result.host = u.substr(0, colon);
            std::string p = (slash == std::string::npos) ? u.substr(colon + 1)
                                                         : u.substr(colon + 1, slash - colon - 1);
            result.port = static_cast<uint16_t>(std::stoi(p));
        } else {
            result.host = (slash == std::string::npos) ? u : u.substr(0, slash);
        }
    } catch (...) {}

    return result;
}

void UnifiedOrchestrator::setupDirectListener() {
    // Start the direct WebSocket listener for chunk notifications
    std::cout << "[Orchestrator] Starting direct WebSocket listener on port 8765\n";
    std::cout << "[Orchestrator] Clients should connect to: ws://127.0.0.1:8765\n";

    // The listener will be started in a separate thread with ChunkTracker integration
    listener_thread_ = std::thread([this]() {
        gpu_listener::run_server_with_tracker(8765, "127.0.0.1", chunk_tracker_.get());
    });
}

bool UnifiedOrchestrator::runBrowserPlusCpp(const Config& cfg) {
    running_ = true;

    // 1) Start direct listener
    setupDirectListener();

    // 2) Spawn processes (use config overrides if provided)
    ChildProcess browser{-1}, cpp{-1};
    SpawnSpec browser_sp;
    if (g_browser_spec && g_browser_spec->enabled) {
        browser = spawn_process(*g_browser_spec);
    } else {
        // Use original server URL for browser
        std::string browser_url = cfg.browser_url;
        browser_sp.argv = { cfg.browser_path, "--new-window", browser_url };
        browser = spawn_process(browser_sp);
    }
    if (browser.pid > 0) {
        std::cout << "[Spawn] Browser pid=" << browser.pid << " connecting to: " << cfg.browser_url << "\n";
        std::cout << "[Spawn] Browser command: ";
        if (g_browser_spec && g_browser_spec->enabled) {
            for (const auto& arg : g_browser_spec->argv) {
                std::cout << arg << " ";
            }
        } else {
            for (const auto& arg : browser_sp.argv) {
                std::cout << arg << " ";
            }
        }
        std::cout << "\n";
    } else {
        std::cerr << "[Spawn] Browser failed (pid=" << browser.pid << ")\n";
    }

    if (g_cpp_spec && g_cpp_spec->enabled) {
        cpp = spawn_process(*g_cpp_spec);
    } else {
        SpawnSpec sp;
        // Use original server URL for C++ client
        sp.argv = { cfg.cpp_client_path };
        std::cout << "[Spawn] C++ client connecting directly to server\n";
        cpp = spawn_process(sp);
    }
    if (cpp.pid > 0) std::cout << "[Spawn] C++ client pid=" << cpp.pid << "\n";
    else std::cerr << "[Spawn] C++ client failed (pid=" << cpp.pid << ")\n";

    // 3) Start collectors
    gpu_collector_.reset(new GPUMetricsCollector(cfg.gpu_index));
    {
        std::vector<pid_t> pids;
        if (browser.pid > 0) pids.push_back(browser.pid);
        if (cpp.pid > 0)     pids.push_back(cpp.pid);
        os_collector_->startMonitoring(pids, cfg.os_monitor_interval_ms);
    }
#if HAVE_CUDA
    gpu_collector_->startMonitoring(cfg.gpu_monitor_interval_ms);
#else
    std::cout << "[GPU] HAVE_CUDA=0 -> GPU/NVML metrics disabled.\n";
#endif

    // 4) Wait loop
    auto t0 = Clock::now();
    while (running_) {
        if (g_interrupted) break;
        if (cfg.duration_sec > 0 && Clock::now() - t0 > std::chrono::seconds(cfg.duration_sec)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 5) Stop collectors and export
    os_collector_->stopMonitoring();
#if HAVE_CUDA
    gpu_collector_->stopMonitoring();
#endif
    exportMetrics(cfg);

    // 6) Best-effort terminate
    if (browser.pid > 0) kill(browser.pid, SIGTERM);
    if (cpp.pid > 0)     kill(cpp.pid, SIGTERM);
    return true;
}

bool UnifiedOrchestrator::runCppOnly(const Config& cfg) {
    running_ = true;

    // Start direct listener
    setupDirectListener();

    ChildProcess cpp{-1};
    if (g_cpp_spec && g_cpp_spec->enabled) {
        cpp = spawn_process(*g_cpp_spec);
    } else {
        SpawnSpec sp;
        sp.argv = { cfg.cpp_client_path };
        cpp = spawn_process(sp);
    }
    if (cpp.pid > 0) std::cout << "[Spawn] C++ client pid=" << cpp.pid << "\n";
    else std::cerr << "[Spawn] C++ client failed (pid=" << cpp.pid << ")\n";
    gpu_collector_.reset(new GPUMetricsCollector(cfg.gpu_index));
    os_collector_->startMonitoring(std::vector<pid_t>{ cpp.pid }, cfg.os_monitor_interval_ms);
#if HAVE_CUDA
    gpu_collector_->startMonitoring(cfg.gpu_monitor_interval_ms);
#else
    std::cout << "[GPU] HAVE_CUDA=0 -> GPU/NVML metrics disabled.\n";
#endif

    auto t0 = Clock::now();
    while (running_) {
        if (g_interrupted) break;
        if (cfg.duration_sec > 0 && Clock::now() - t0 > std::chrono::seconds(cfg.duration_sec)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    os_collector_->stopMonitoring();
#if HAVE_CUDA
    gpu_collector_->stopMonitoring();
#endif
    exportMetrics(cfg);

    if (cpp.pid > 0) kill(cpp.pid, SIGTERM);
    return true;
}

void UnifiedOrchestrator::stop() { running_ = false; }

static inline double tp_secs(const Clock::time_point& tp) {
    return std::chrono::duration<double>(tp.time_since_epoch()).count();
}

static std::vector<OSMetrics> filter_os(const std::vector<OSMetrics>& in, double t0, double t1) {
    std::vector<OSMetrics> out; out.reserve(in.size());
    for (const auto& m : in) if (m.timestamp >= t0 && m.timestamp <= t1) out.push_back(m);
    return out;
}
#if HAVE_CUDA
static std::vector<GPUMetrics> filter_gpu(const std::vector<GPUMetrics>& in, double t0, double t1) {
    std::vector<GPUMetrics> out; out.reserve(in.size());
    for (const auto& m : in) if (m.timestamp >= t0 && m.timestamp <= t1) out.push_back(m);
    return out;
}
#endif

void UnifiedOrchestrator::exportMetrics(const Config& cfg) {
    std::filesystem::create_directories(cfg.output_dir);
    const auto os_all  = os_collector_->getMetrics();
#if HAVE_CUDA
    const auto gpu_all = gpu_collector_->getMetrics();
#endif

    auto chunks = chunk_tracker_->getAllChunks();
    for (auto& ch : chunks) {
        const double t_begin = tp_secs(ch.arrival_time);
        double t_end = tp_secs(ch.completion_time);
        if (t_end <= 0.0) t_end = tp_secs(Clock::now());
        chunk_tracker_->attachOSMetrics(ch.chunk_id, filter_os(os_all, t_begin, t_end));
#if HAVE_CUDA
        chunk_tracker_->attachGPUMetrics(ch.chunk_id, filter_gpu(gpu_all, t_begin, t_end));
#endif
    }

    chunk_tracker_->exportToCSV(cfg.output_dir + "/chunks.csv");

    if (cfg.export_detailed_samples) {
        {   // OS raw
            std::ofstream f(cfg.output_dir + "/os_samples.csv");
            f << "timestamp,pid,cpu_percent,mem_rss_kb,mem_vms_kb,disk_read_bytes,disk_write_bytes,net_recv_bytes,net_sent_bytes\n";
            for (auto& m : os_all) {
                f << m.timestamp << "," << m.pid << "," << m.cpu_percent << ","
                  << m.mem_rss_kb << "," << m.mem_vms_kb << ","
                  << m.disk_read_bytes << "," << m.disk_write_bytes << ","
                  << m.net_recv_bytes << "," << m.net_sent_bytes << "\n";
            }
        }
#if HAVE_CUDA
        {   // GPU raw
            std::ofstream f(cfg.output_dir + "/gpu_samples.csv");
            f << "timestamp,gpu_index,power_mw,gpu_util_percent,mem_util_percent,mem_used_bytes,sm_clock_mhz\n";
            for (auto& m : gpu_all) {
                f << m.timestamp << "," << m.gpu_index << "," << m.power_mw << ","
                  << m.gpu_util_percent << "," << m.mem_util_percent << ","
                  << m.mem_used_bytes << "," << m.sm_clock_mhz << "\n";
            }
        }
#endif
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

static bool load_config(const std::string& path, UnifiedOrchestrator::Config& cfg_out) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "[Config] Cannot open: " << path << "\n";
        return false;
    }
    json j; f >> j;

    if (j.contains("mode") && j["mode"].is_string()) {
        // mode handled in main() after parse
    }
    if (j.contains("server")) cfg_out.server_path = j["server"].get<std::string>();
    if (j.contains("browser_url")) cfg_out.browser_url = j["browser_url"].get<std::string>();
    if (j.contains("gpu_index")) cfg_out.gpu_index = j["gpu_index"].get<unsigned>();
    if (j.contains("os_interval_ms"))  cfg_out.os_monitor_interval_ms  = j["os_interval_ms"].get<unsigned>();
    if (j.contains("gpu_interval_ms")) cfg_out.gpu_monitor_interval_ms = j["gpu_interval_ms"].get<unsigned>();
    if (j.contains("duration_sec")) cfg_out.duration_sec = j["duration_sec"].get<int>();
    if (j.contains("output_dir")) cfg_out.output_dir = j["output_dir"].get<std::string>();
    if (j.contains("export_detailed_samples")) cfg_out.export_detailed_samples = j["export_detailed_samples"].get<bool>();
    auto parse_proc = [](const json& jp) -> SpawnSpec {
        SpawnSpec sp;
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
        g_browser_spec = parse_proc(j["browser"]);
    }
    if (j.contains("cpp_client") && j["cpp_client"].is_object()) {
        g_cpp_spec = parse_proc(j["cpp_client"]);
    }

    // For backward-compat: if no spec provided, leave nullptr and CLI fields will be used.
    return true;
}

} // namespace unified_monitor

int main(int argc, char** argv) {
    using namespace unified_monitor;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    UnifiedOrchestrator::Config cfg;
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
        else if (a == "--browser-path") cfg.browser_path = need("--browser-path");
        else if (a == "--browser-url") cfg.browser_url = need("--browser-url");
        else if (a == "--cpp-client") cfg.cpp_client_path = need("--cpp-client");
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
        std::ifstream cf(*config_path);
        if (!cf.good()) {
            std::cerr << "[Config] file not found: " << *config_path << "\n";
            return 2;
        }
        json j; cf >> j;
        // If config specifies mode, override CLI/default
        if (j.contains("mode") && j["mode"].is_string()) mode = j["mode"].get<std::string>();
        // Parse the rest
        load_config(*config_path, cfg);
    }

    UnifiedOrchestrator orch;

    bool ok = false;
    if (mode == "browser+cpp") ok = orch.runBrowserPlusCpp(cfg);
    else if (mode == "cpp-only") ok = orch.runCppOnly(cfg);
    else { std::cerr << "Invalid --mode\n"; return 2; }

    orch.stop();
    return ok ? 0 : 1;
}
