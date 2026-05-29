#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "metrics_storage.hpp"
#include "websocket_listener.hpp"

namespace unified_monitor {

struct EnergyMetrics {
    int64_t ts_unix_ns = 0;
    int zone_index = 0;
    std::string zone_name;
    uint64_t energy_delta_uj = 0;
    uint64_t total_energy_uj = 0;
};

class RAPLEnergyCollector {
public:
    RAPLEnergyCollector();
    ~RAPLEnergyCollector();

    bool isAvailable() const;
    std::vector<EnergyMetrics> collect();

private:
    struct Zone {
        std::string energy_path;
        std::string name;
        uint64_t max_range_uj = 0;
        uint64_t prev_energy_uj = 0;
        bool has_prev = false;
    };

    std::vector<Zone> zones_;
    bool available_ = false;
};

struct GpuPowerSample {
    unsigned gpu_index = 0;
    int64_t power_mw = -1;
};

class SimpleOrchestrator {
public:
    struct Config {
        std::string label;
        std::string output_dir = "./metrics";
        unsigned gpu_index = 0;
        unsigned interval_ms = 100;
        int duration_sec = 0;
        std::string websocket_url;
        bool wait_for_start = false;
        MetricsStorage::Config storage_config;
    };

    SimpleOrchestrator();
    ~SimpleOrchestrator();

    bool run(const Config& cfg);
    void stop();

private:
    void setupWebSocket(const Config& cfg);
    void sampleOnce(const Config& cfg, uint64_t sample_index);
    GpuPowerSample sampleGpu(unsigned gpu_index);

    std::unique_ptr<RAPLEnergyCollector> rapl_collector_;
    std::unique_ptr<MetricsStorage> storage_;
    std::unique_ptr<WebSocketListener> websocket_listener_;

    std::atomic<bool> running_{false};
    std::atomic<bool> sampling_{false};
};

} // namespace unified_monitor
