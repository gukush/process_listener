#include "simple_orchestrator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

#if HAVE_CUDA
  #if __has_include(<nvml.h>)
    #include <nvml.h>
  #else
    #include <nvidia-ml/nvml.h>
  #endif
#endif

namespace {

std::atomic<bool> g_interrupted{false};

void handleSignal(int) {
    g_interrupted = true;
}

int64_t unixNowNs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string hostnameLabel() {
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname) - 1) == 0 && hostname[0] != '\0') {
        return hostname;
    }
    return "unknown";
}

#if HAVE_CUDA
std::string nvmlError(nvmlReturn_t status) {
    const char* text = nvmlErrorString(status);
    return text ? text : "NVML error";
}
#endif

} // namespace

namespace unified_monitor {

SimpleOrchestrator::SimpleOrchestrator() = default;

SimpleOrchestrator::~SimpleOrchestrator() {
    stop();
}

void SimpleOrchestrator::stop() {
    running_ = false;
    sampling_ = false;
    if (websocket_listener_) {
        websocket_listener_->disconnect();
    }
    if (storage_) {
        storage_->flushAndClose();
    }
}

void SimpleOrchestrator::setupWebSocket(const Config& cfg) {
    websocket_listener_ = std::make_unique<WebSocketListener>();
    websocket_listener_->onStartMetrics = [this]() {
        sampling_ = true;
        std::cout << "[Monitor] metrics:start received, sampling enabled" << std::endl;
    };
    websocket_listener_->onStopMetrics = [this]() {
        sampling_ = false;
        if (storage_) storage_->flush();
        std::cout << "[Monitor] metrics:stop received, sampling paused" << std::endl;
    };

    if (!websocket_listener_->connect(cfg.websocket_url)) {
        throw std::runtime_error("failed to connect WebSocket listener");
    }
}

GpuPowerSample SimpleOrchestrator::sampleGpu(unsigned gpu_index) {
    GpuPowerSample sample{};
    sample.gpu_index = gpu_index;

#if HAVE_CUDA
    static bool initialized = false;
    static bool available = false;
    static unsigned current_index = 0;
    static nvmlDevice_t device{};

    if (!initialized || current_index != gpu_index) {
        if (initialized) {
            nvmlShutdown();
        }
        initialized = true;
        current_index = gpu_index;
        available = false;

        nvmlReturn_t status = nvmlInit_v2();
        if (status != NVML_SUCCESS) {
            std::cerr << "[NVML] init failed: " << nvmlError(status) << std::endl;
            return sample;
        }
        status = nvmlDeviceGetHandleByIndex_v2(gpu_index, &device);
        if (status != NVML_SUCCESS) {
            std::cerr << "[NVML] device " << gpu_index << " unavailable: "
                      << nvmlError(status) << std::endl;
            nvmlShutdown();
            return sample;
        }
        available = true;
        std::cout << "[NVML] Sampling GPU " << gpu_index << " power usage" << std::endl;
    }

    if (!available) return sample;

    unsigned int power = 0;
    if (nvmlDeviceGetPowerUsage(device, &power) == NVML_SUCCESS) {
        sample.power_mw = static_cast<int64_t>(power);
    }

#endif

    return sample;
}

void SimpleOrchestrator::sampleOnce(const Config& cfg, uint64_t sample_index) {
    const int64_t ts_unix_ns = unixNowNs();
    const auto gpu = sampleGpu(cfg.gpu_index);
    auto energy = rapl_collector_ ? rapl_collector_->collect() : std::vector<EnergyMetrics>{};

    std::vector<UnifiedMetricSample> rows;
    if (energy.empty()) {
        UnifiedMetricSample row{};
        row.ts_unix_ns = ts_unix_ns;
        row.sample_index = sample_index;
        row.label = cfg.label;
        row.gpu_index = gpu.gpu_index;
        row.gpu_power_mw = gpu.power_mw;
        row.rapl_zone_index = -1;
        row.rapl_zone_name = "unavailable";
        rows.push_back(std::move(row));
    } else {
        rows.reserve(energy.size());
        for (const auto& e : energy) {
            UnifiedMetricSample row{};
            row.ts_unix_ns = ts_unix_ns;
            row.sample_index = sample_index;
            row.label = cfg.label;
            row.gpu_index = gpu.gpu_index;
            row.gpu_power_mw = gpu.power_mw;
            row.rapl_zone_index = e.zone_index;
            row.rapl_zone_name = e.zone_name;
            row.cpu_energy_delta_uj = e.energy_delta_uj;
            row.cpu_energy_total_uj = e.total_energy_uj;
            rows.push_back(std::move(row));
        }
    }

    storage_->addSamples(rows);
}

bool SimpleOrchestrator::run(const Config& input_cfg) {
    Config cfg = input_cfg;
    if (cfg.label.empty()) cfg.label = hostnameLabel();
    if (cfg.interval_ms == 0) cfg.interval_ms = 100;

    MetricsStorage::Config storage_cfg = cfg.storage_config;
    storage_cfg.output_dir = cfg.output_dir;
    storage_cfg.label = cfg.label;

    storage_ = std::make_unique<MetricsStorage>(storage_cfg);
    if (!storage_->initialize()) {
        return false;
    }

    rapl_collector_ = std::make_unique<RAPLEnergyCollector>();
    if (!rapl_collector_->isAvailable()) {
        std::cerr << "[RAPL] No readable CPU energy zones; rows will mark RAPL unavailable" << std::endl;
    }

    running_ = true;
    sampling_ = !cfg.wait_for_start;

    if (cfg.wait_for_start) {
        if (cfg.websocket_url.empty()) {
            std::cerr << "[Monitor] --wait-for-start requires --url" << std::endl;
            return false;
        }
        try {
            setupWebSocket(cfg);
        } catch (const std::exception& e) {
            std::cerr << "[Monitor] WebSocket setup failed: " << e.what() << std::endl;
            return false;
        }
        std::cout << "[Monitor] Waiting for metrics:start" << std::endl;
    } else {
        std::cout << "[Monitor] Sampling immediately every " << cfg.interval_ms << " ms" << std::endl;
    }

    const auto started = std::chrono::steady_clock::now();
    auto next_tick = started;
    uint64_t sample_index = 0;

    while (running_ && !::g_interrupted.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (cfg.duration_sec > 0 && now - started >= std::chrono::seconds(cfg.duration_sec)) {
            break;
        }

        if (sampling_.load()) {
            sampleOnce(cfg, sample_index++);
        }

        next_tick += std::chrono::milliseconds(cfg.interval_ms);
        std::this_thread::sleep_until(next_tick);
        if (std::chrono::steady_clock::now() > next_tick + std::chrono::milliseconds(cfg.interval_ms)) {
            next_tick = std::chrono::steady_clock::now();
        }
    }

    stop();
    return true;
}

} // namespace unified_monitor

namespace {

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "  --label LABEL           Machine/run label; defaults to hostname\n"
        << "  --out-dir DIR           Output directory (default: ./metrics)\n"
        << "  --gpu-index N           NVML GPU index (default: 0)\n"
        << "  --interval MS           Sampling interval (default: 100)\n"
        << "  --duration SEC          Stop after SEC seconds (default: until signal)\n"
        << "  --wait-for-start        Wait for WebSocket metrics:start before sampling\n"
        << "  --url URL               WebSocket URL used with --wait-for-start\n"
        << "  --max-buffered-samples N Flush after N buffered rows (default: 100000)\n"
        << "  --flush-interval-sec N  Finalize current ORC file every N seconds (default: 300)\n"
        << "  --no-zstd               Disable ORC Zstd compression\n"
        << "  --help                  Show this help\n";
}

bool parseUnsigned(const std::string& text, unsigned& out) {
    try {
        size_t pos = 0;
        unsigned long value = std::stoul(text, &pos, 10);
        if (pos != text.size()) return false;
        out = static_cast<unsigned>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool parseInt(const std::string& text, int& out) {
    try {
        size_t pos = 0;
        int value = std::stoi(text, &pos, 10);
        if (pos != text.size()) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseSize(const std::string& text, size_t& out) {
    try {
        size_t pos = 0;
        unsigned long long value = std::stoull(text, &pos, 10);
        if (pos != text.size()) return false;
        out = static_cast<size_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    unified_monitor::SimpleOrchestrator::Config cfg;
    cfg.interval_ms = 100;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto requireValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        try {
            if (arg == "--help" || arg == "-h") {
                usage(argv[0]);
                return 0;
            } else if (arg == "--label") {
                cfg.label = requireValue("--label");
            } else if (arg == "--out-dir") {
                cfg.output_dir = requireValue("--out-dir");
            } else if (arg == "--gpu-index") {
                unsigned value = 0;
                if (!parseUnsigned(requireValue("--gpu-index"), value)) {
                    throw std::runtime_error("invalid --gpu-index");
                }
                cfg.gpu_index = value;
            } else if (arg == "--interval" || arg == "--gpu-interval" || arg == "--os-interval") {
                unsigned value = 0;
                if (!parseUnsigned(requireValue(arg.c_str()), value)) {
                    throw std::runtime_error("invalid interval");
                }
                cfg.interval_ms = value;
            } else if (arg == "--duration") {
                int value = 0;
                if (!parseInt(requireValue("--duration"), value)) {
                    throw std::runtime_error("invalid --duration");
                }
                cfg.duration_sec = value;
            } else if (arg == "--wait-for-start") {
                cfg.wait_for_start = true;
            } else if (arg == "--no-wait") {
                cfg.wait_for_start = false;
            } else if (arg == "--url") {
                cfg.websocket_url = requireValue("--url");
            } else if (arg == "--max-buffered-samples") {
                size_t value = 0;
                if (!parseSize(requireValue("--max-buffered-samples"), value)) {
                    throw std::runtime_error("invalid --max-buffered-samples");
                }
                cfg.storage_config.max_buffered_samples = value;
            } else if (arg == "--flush-interval-sec") {
                unsigned value = 0;
                if (!parseUnsigned(requireValue("--flush-interval-sec"), value)) {
                    throw std::runtime_error("invalid --flush-interval-sec");
                }
                cfg.storage_config.max_flush_interval = std::chrono::seconds(value);
            } else if (arg == "--no-zstd") {
                cfg.storage_config.use_zstd_compression = false;
            } else {
                throw std::runtime_error("unknown option: " + arg);
            }
        } catch (const std::exception& e) {
            std::cerr << e.what() << std::endl;
            usage(argv[0]);
            return 2;
        }
    }

    unified_monitor::SimpleOrchestrator orchestrator;
    return orchestrator.run(cfg) ? 0 : 1;
}
