// main.cpp - Unified orchestrator entry with HAVE_CUDA

//   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHAVE_CUDA=1
//   cmake --build build -j
//
//   ./build/unified_monitor --mode browser+cpp --server ws://127.0.0.1:8765 \
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

#include <nlohmann/json.hpp>

#if HAVE_CUDA
  #include <nvml.h>
#endif

#include "orchestrator.cpp"
#include "websocket_proxy.hpp"

using json = nlohmann::json;

namespace unified_monitor {

static std::atomic<bool> g_interrupted = false;
static void handle_signal(int) { g_interrupted = true; }

struct ChildProcess {
    pid_t pid = -1;
};

static ChildProcess spawn_process(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return {};
    }
    if (pid == 0) {
        execvp(cargv[0], cargv.data());
        perror("execvp");
        _exit(127);
    }
    return {pid};
}

class EnhancedMessageInterceptor {
public:
    explicit EnhancedMessageInterceptor(ChunkTracker* tracker)
        : tracker_(tracker) {}

    void onTaskInit(const json& msg) {
        (void)msg;
        // idk if it should be continued
    }

    void onChunkAssign(const json& msg) {
        try {
            std::string chunkId = msg.at("chunkId").get<std::string>();
            std::string taskId  = msg.value("taskId", std::string{});
            tracker_->onChunkArrival(chunkId, taskId);
            tracker_->onChunkStart(chunkId);
        } catch (const std::exception& e) {
            std::cerr << "[Interceptor] onChunkAssign parse error: " << e.what() << "\n";
        }
    }

    void onChunkResult(const json& msg) {
        try {
            std::string chunkId = msg.at("chunkId").get<std::string>();
            std::string status  = msg.value("status", "completed");
            tracker_->onChunkComplete(chunkId, status);
        } catch (const std::exception& e) {
            std::cerr << "[Interceptor] onChunkResult parse error: " << e.what() << "\n";
        }
    }

private:
    ChunkTracker* tracker_;
};

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
                } catch (...) {
                    // ignore missing/zombie PIDs
                }
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

// GPUMetricsCollector
#if HAVE_CUDA

static std::string nvml_err_str(nvmlReturn_t st) {
    const char* s = nvmlErrorString(st);
    return s ? std::string(s) : "NVML_ERROR";
}

GPUMetricsCollector::GPUMetricsCollector(unsigned gpu_index)
    : gpu_index_(gpu_index) {}

GPUMetricsCollector::~GPUMetricsCollector() { stopMonitoring(); }

void GPUMetricsCollector::startMonitoring(unsigned interval_ms) {
    stopMonitoring();
    interval_ms_ = interval_ms ? interval_ms : 50;
    running_ = true;
    worker_ = std::thread([this]() {
        nvmlReturn_t st = nvmlInit_v2();
        if (st != NVML_SUCCESS) {
            std::cerr << "[NVML] init failed: " << nvml_err_str(st) << "\n";
            running_ = false;
            return;
        }
        nvmlDevice_t dev{};
        st = nvmlDeviceGetHandleByIndex_v2(gpu_index_, &dev);
        if (st != NVML_SUCCESS) {
            std::cerr << "[NVML] device index " << gpu_index_ << " error: " << nvml_err_str(st) << "\n";
            nvmlShutdown();
            running_ = false;
            return;
        }

        while (running_) {
            auto t = Clock::now();
            GPUMetrics m{};
            m.timestamp = std::chrono::duration<double>(t.time_since_epoch()).count();
            m.gpu_index = gpu_index_;

            // Power (mW)
            unsigned int power = 0;
            if (nvmlDeviceGetPowerUsage(dev, &power) == NVML_SUCCESS) m.power_mw = power;

            // Utilization %
            nvmlUtilization_t util{};
            if (nvmlDeviceGetUtilizationRates(dev, &util) == NVML_SUCCESS) {
                m.gpu_util_percent = util.gpu;
                m.mem_util_percent = util.memory;
            }

            // Memory used
            nvmlMemory_t mem{};
            if (nvmlDeviceGetMemoryInfo(dev, &mem) == NVML_SUCCESS) {
                m.mem_used_bytes = mem.used;
            }

            // SM clock
            unsigned int sm = 0;
            if (nvmlDeviceGetClockInfo(dev, NVML_CLOCK_SM, &sm) == NVML_SUCCESS) {
                m.sm_clock_mhz = sm;
            }

            // Per-process GPU utilization (newer API)
            const unsigned int MAX_SAMPLES = 1024;
            std::vector<nvmlProcessUtilizationSample_t> samples(MAX_SAMPLES);
            unsigned int n = MAX_SAMPLES;
            st = nvmlDeviceGetProcessUtilization(dev, samples.data(), &n, 0);
            if (st == NVML_SUCCESS) {
                for (unsigned int i = 0; i < n; ++i) {
                    const auto& s = samples[i];
                    m.pid_gpu_percent[static_cast<unsigned int>(s.pid)] = s.smUtil;
                }
            } else {
                // Fallback for older drivers (no % available)
                unsigned int count = 128;
                std::vector<nvmlProcessInfo_t> procs(count);
                st = nvmlDeviceGetComputeRunningProcesses(dev, &count, procs.data());
                if (st == NVML_SUCCESS) {
                    for (unsigned int i = 0; i < count; ++i) {
                        m.pid_gpu_percent[static_cast<unsigned int>(procs[i].pid)] = 0;
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lk(mx_);
                samples_.push_back(std::move(m));
            }

            std::this_thread::sleep_until(t + std::chrono::milliseconds(interval_ms_));
        }

        nvmlShutdown();
    });
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

#else

GPUMetricsCollector::GPUMetricsCollector(unsigned gpu_index)
    : gpu_index_(gpu_index) {}

GPUMetricsCollector::~GPUMetricsCollector() { stopMonitoring(); }

void GPUMetricsCollector::startMonitoring(unsigned /*interval_ms*/) {
    running_ = false;
}

void GPUMetricsCollector::stopMonitoring() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

std::vector<GPUMetrics> GPUMetricsCollector::getMetrics() const {
    return {}; // no GPU samples
}

#endif // HAVE_CUDA

UnifiedOrchestrator::UnifiedOrchestrator()
    : os_collector_(std::make_unique<OSMetricsCollector>()),
      gpu_collector_(std::make_unique<GPUMetricsCollector>(0)),
      chunk_tracker_(std::make_unique<ChunkTracker>()),
      interceptor_(nullptr) {}

UnifiedOrchestrator::~UnifiedOrchestrator() {
    stop();
}

void UnifiedOrchestrator::setupMessageProxy(const std::string& upstreamUrl) {
    std::string host = "127.0.0.1";
    uint16_t port = 8765;
    try {
        std::string u = upstreamUrl;
        auto pos_ws = u.find("://");
        if (pos_ws != std::string::npos) u = u.substr(pos_ws + 3);
        auto colon = u.find(':');
        auto slash = u.find('/');
        if (colon != std::string::npos) {
            host = u.substr(0, colon);
            std::string p = (slash == std::string::npos) ? u.substr(colon + 1)
                                                         : u.substr(colon + 1, slash - colon - 1);
            port = static_cast<uint16_t>(std::stoi(p));
        } else {
            host = (slash == std::string::npos) ? u : u.substr(0, slash);
        }
    } catch (...) {}

    const uint16_t listen_port = 9797;

    static std::unique_ptr<BeastWebSocketProxy> s_proxy;
    if (!s_proxy) s_proxy.reset(new BeastWebSocketProxy(chunk_tracker_.get(),
                                                        new EnhancedMessageInterceptor(chunk_tracker_.get())));
    bool ok = s_proxy->start(listen_port, host, port);
    if (!ok) {
        std::cerr << "[Orchestrator] Failed to start WebSocket proxy on :" << listen_port
                  << " to " << host << ":" << port << "\n";
    } else {
        std::cout << "[Orchestrator] WebSocket proxy listening on :" << listen_port
                  << " upstream " << host << ":" << port << "\n";
    }
}

bool UnifiedOrchestrator::runBrowserPlusCpp(const Config& cfg) {
    running_ = true;

    setupMessageProxy(cfg.server_path);

    std::vector<std::string> browser_argv = {
        cfg.browser_path,
        "--new-window",
        cfg.browser_url
    };
    auto browser = spawn_process(browser_argv);
    if (browser.pid <= 0) {
        std::cerr << "[Spawn] Failed to start browser at " << cfg.browser_path << "\n";
    } else {
        std::cout << "[Spawn] Browser pid=" << browser.pid << "\n";
    }

    std::vector<std::string> cpp_argv = { cfg.cpp_client_path };
    auto cpp = spawn_process(cpp_argv);
    if (cpp.pid <= 0) {
        std::cerr << "[Spawn] Failed to start C++ client at " << cfg.cpp_client_path << "\n";
    } else {
        std::cout << "[Spawn] C++ client pid=" << cpp.pid << "\n";
    }

    gpu_collector_.reset(new GPUMetricsCollector(cfg.gpu_index));

    {
        std::vector<pid_t> pids;
        if (browser.pid > 0) pids.push_back(browser.pid);
        if (cpp.pid > 0)     pids.push_back(cpp.pid);
        os_collector_->startMonitoring(pids, cfg.os_monitor_interval_ms);
    }

#if HAVE_CUDA
    gpu_collector_->stopMonitoring();
    gpu_collector_->startMonitoring(cfg.gpu_monitor_interval_ms);
#else
    std::cout << "[GPU] HAVE_CUDA=0 -> GPU/NVML metrics disabled.\n";
#endif

    auto t0 = Clock::now();
    while (running_) {
        if (g_interrupted) break;
        if (cfg.duration_sec > 0 &&
            Clock::now() - t0 > std::chrono::seconds(cfg.duration_sec)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    os_collector_->stopMonitoring();
#if HAVE_CUDA
    gpu_collector_->stopMonitoring();
#endif

    exportMetrics(cfg);
    if (browser.pid > 0) kill(browser.pid, SIGTERM);
    if (cpp.pid > 0)     kill(cpp.pid, SIGTERM);

    return true;
}

bool UnifiedOrchestrator::runCppOnly(const Config& cfg) {
    running_ = true;

    // Spawn native client (direct-to-server path could be added here)
    std::vector<std::string> cpp_argv = { cfg.cpp_client_path };
    auto cpp = spawn_process(cpp_argv);
    if (cpp.pid <= 0) {
        std::cerr << "[Spawn] Failed to start C++ client at " << cfg.cpp_client_path << "\n";
    } else {
        std::cout << "[Spawn] C++ client pid=" << cpp.pid << "\n";
    }

    // Recreate GPU collector for requested index
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
        if (cfg.duration_sec > 0 &&
            Clock::now() - t0 > std::chrono::seconds(cfg.duration_sec)) break;
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

void UnifiedOrchestrator::stop() {
    running_ = false;
}

static inline double tp_to_secs(const Clock::time_point& tp) {
    return std::chrono::duration<double>(tp.time_since_epoch()).count();
}

static std::vector<OSMetrics> filter_os(const std::vector<OSMetrics>& in,
                                        double t0, double t1) {
    std::vector<OSMetrics> out;
    for (const auto& m : in) if (m.timestamp >= t0 && m.timestamp <= t1) out.push_back(m);
    return out;
}
#if HAVE_CUDA
static std::vector<GPUMetrics> filter_gpu(const std::vector<GPUMetrics>& in,
                                          double t0, double t1) {
    std::vector<GPUMetrics> out;
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
        const double t_begin = tp_to_secs(ch.arrival_time);
        double t_end = tp_to_secs(ch.completion_time);
        if (t_end <= 0.0) t_end = tp_to_secs(Clock::now());

        auto os_win = filter_os(os_all, t_begin, t_end);
        chunk_tracker_->attachOSMetrics(ch.chunk_id, os_win);

#if HAVE_CUDA
        auto gpu_win = filter_gpu(gpu_all, t_begin, t_end);
        chunk_tracker_->attachGPUMetrics(ch.chunk_id, gpu_win);
#endif
    }

    chunk_tracker_->exportToCSV(cfg.output_dir + "/chunks.csv");

    if (cfg.export_detailed_samples) {
        {
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
        {
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

// CLI
static void print_usage(const char* argv0) {
    std::cerr <<
    "Usage:\n"
    "  " << argv0 << " --mode [browser+cpp|cpp-only]\n"
    "               --server ws://HOST:PORT\n"
    "               [--browser-path PATH] [--browser-url URL]\n"
    "               [--cpp-client PATH]\n"
    "               [--gpu-index N] [--os-interval MS] [--gpu-interval MS]\n"
    "               [--duration SEC] [--out-dir DIR]\n";
}

} // namespace unified_monitor

int main(int argc, char** argv) {
    using namespace unified_monitor;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    UnifiedOrchestrator::Config cfg;
    std::string mode = "browser+cpp";
    cfg.server_path = "ws://127.0.0.1:8765";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) {
            if (i + 1 >= argc) { std::cerr << name << " requires value\n"; print_usage(argv[0]); std::exit(2); }
            return std::string(argv[++i]);
        };
        if (a == "--mode") mode = need("--mode");
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
        else {
            std::cerr << "Unknown arg: " << a << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    UnifiedOrchestrator orch;

    bool ok = false;
    if (mode == "browser+cpp") ok = orch.runBrowserPlusCpp(cfg);
    else if (mode == "cpp-only") ok = orch.runCppOnly(cfg);
    else { std::cerr << "Invalid --mode\n"; return 2; }

    orch.stop();
    return ok ? 0 : 1;
}
