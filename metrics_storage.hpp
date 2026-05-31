#pragma once

#include <orc/OrcFile.hh>
#include <orc/Writer.hh>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace unified_monitor {

struct UnifiedMetricSample {
    int64_t ts_unix_ns = 0;
    uint64_t sample_index = 0;
    std::string label;
    unsigned gpu_index = 0;
    int64_t gpu_power_mw = -1;
    int rapl_zone_index = -1;
    std::string rapl_zone_name;
    uint64_t cpu_energy_delta_uj = 0;
    uint64_t cpu_energy_total_uj = 0;
};

class MetricsStorage {
public:
    struct Config {
        std::string output_dir = "./metrics";
        std::string label;
        bool use_zstd_compression = true;
        size_t max_buffered_samples = 100000;
        std::chrono::milliseconds max_flush_interval{std::chrono::minutes(5)};
    };

    explicit MetricsStorage(const Config& config);
    MetricsStorage();
    ~MetricsStorage();

    bool initialize();
    void setStorageConfig(const Config& config);
    void addSamples(const std::vector<UnifiedMetricSample>& samples);
    void flush();
    void flushAndClose();

    struct StorageStats {
        size_t total_samples = 0;
        size_t files_written = 0;
        std::string filename;
    };
    StorageStats getStats() const;

private:
    struct FileWriter {
        std::unique_ptr<orc::Writer> writer;
        std::unique_ptr<orc::OutputStream> output;
        std::unique_ptr<orc::Type> schema;
        std::string filename;
        size_t row_count = 0;
    };

    void createFile();
    void flushData();
    void closeFile();
    std::string generateFilename() const;
    std::unique_ptr<orc::Type> createSchema() const;
    void writeBatch(orc::Writer* writer, const std::vector<UnifiedMetricSample>& samples);

    Config config_;
    mutable std::mutex mutex_;
    std::unique_ptr<FileWriter> file_;
    std::vector<UnifiedMetricSample> buffer_;
    StorageStats stats_;
    std::chrono::steady_clock::time_point last_flush_time_;
    std::atomic<bool> closed_{false};
};

} // namespace unified_monitor
