// rapl_energy_linux.cpp
// RAPL (Running Average Power Limit) energy counter reader for Intel CPUs.
// Discovers zones under /sys/class/powercap/intel-rapl* and reports deltas
// in microjoules, handling counter wrap-around via max_energy_range_uj.
//
// IMPORTANT: sysfs virtual files are reopened on each sample. Keeping
// energy_uj open through stdio can return stale values on some systems.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "simple_orchestrator.hpp"

namespace unified_monitor {

namespace {

static bool readUint64FromPath(const std::string& path, uint64_t& out) {
    std::ifstream f(path);
    if (!f) return false;
    unsigned long long v = 0;
    if (!(f >> v)) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

static std::string readStringFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

} // namespace

RAPLEnergyCollector::RAPLEnergyCollector() {
    const std::filesystem::path base = "/sys/class/powercap";
    if (!std::filesystem::exists(base)) {
        std::cout << "[RAPL] /sys/class/powercap not found, RAPL unavailable" << std::endl;
        return;
    }

    int zone_index = 0;
    for (const auto& entry : std::filesystem::directory_iterator(base)) {
        if (!entry.is_directory()) continue;
        const std::string dirname = entry.path().filename().string();

        // Accept intel-rapl:N or intel-rapl-mmio:N, but NOT the top-level "intel-rapl" container
        if (!(dirname.rfind("intel-rapl:", 0) == 0 || dirname.rfind("intel-rapl-mmio:", 0) == 0))
            continue;

        const std::string energy_path = entry.path() / "energy_uj";
        if (!std::filesystem::exists(energy_path)) continue;

        // Verify we can actually read it
        uint64_t test_val = 0;
        if (!readUint64FromPath(energy_path, test_val)) {
            std::cout << "[RAPL] Zone " << dirname << " energy_uj read failed" << std::endl;
            continue;
        }

        Zone z;
        z.energy_path = energy_path;
        z.name = readStringFromFile(entry.path() / "name");
        if (z.name.empty()) z.name = dirname;

        // Read max range for wrap handling (one-shot, file kept open not needed afterward)
        uint64_t max_range = 0;
        if (readUint64FromPath(entry.path() / "max_energy_range_uj", max_range)) {
            z.max_range_uj = max_range;
        } else {
            z.max_range_uj = (1ULL << 32); // conservative fallback
            std::cout << "[RAPL] Warning: max_energy_range_uj missing for " << dirname
                      << ", using fallback 2^32" << std::endl;
        }

        zones_.push_back(std::move(z));
        std::cout << "[RAPL] Discovered zone [" << zone_index << "] " << z.name
                  << " (" << dirname << ", max_range=" << z.max_range_uj << " uJ)" << std::endl;
        zone_index++;
    }

    available_ = !zones_.empty();
    if (available_) {
        std::cout << "[RAPL] Energy monitoring active with " << zones_.size() << " zone(s)" << std::endl;
    } else {
        std::cout << "[RAPL] No readable RAPL zones found" << std::endl;
    }
}

RAPLEnergyCollector::~RAPLEnergyCollector() = default;

bool RAPLEnergyCollector::isAvailable() const {
    return available_;
}

std::vector<EnergyMetrics> RAPLEnergyCollector::collect() {
    std::vector<EnergyMetrics> results;
    if (!available_) return results;

    const auto now_sys = std::chrono::system_clock::now();
    const auto ts_ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(now_sys)
                           .time_since_epoch().count();
    const int64_t ts_unix_ns = static_cast<int64_t>(ts_ns);

    for (size_t i = 0; i < zones_.size(); ++i) {
        Zone& z = zones_[i];
        if (z.energy_path.empty()) continue;

        uint64_t current = 0;
        if (!readUint64FromPath(z.energy_path, current)) {
            continue; // skip if unreadable this time
        }

        uint64_t delta = 0;
        if (z.has_prev) {
            if (current >= z.prev_energy_uj) {
                delta = current - z.prev_energy_uj;
            } else {
                // Wrap-around detected
                if (z.max_range_uj > 0) {
                    delta = (z.max_range_uj - z.prev_energy_uj) + current;
                } else {
                    delta = current; // fallback
                }
            }
        }

        z.prev_energy_uj = current;
        z.has_prev = true;

        EnergyMetrics m{};
        m.ts_unix_ns = ts_unix_ns;
        m.zone_index = static_cast<int>(i);
        m.zone_name = z.name;
        m.energy_delta_uj = delta;
        m.total_energy_uj = current;
        results.push_back(std::move(m));
    }

    return results;
}

} // namespace unified_monitor
