#pragma once

#include <orc/OrcFile.hh>
#include <orc/Reader.hh>
#include <orc/Writer.hh>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <atomic>

namespace unified_monitor {

// Forward declarations
struct OSMetrics;
struct GPUMetrics;

// Efficient metrics storage using Apache ORC with Zstd compression
class MetricsStorage {
public:
    struct Config {
        std::string output_dir = "./metrics";
        size_t max_rows_per_file = 1000000;  // Roll files every ~100k rows
        std::chrono::minutes max_file_age{5}; // Roll files every 5 minutes
        bool use_zstd_compression = true;
        int zstd_compression_level = 3; // Good balance of speed vs compression
    };

    // Fix: Use default constructor approach instead of default parameter with Config{}
    explicit MetricsStorage(const Config& config);
    MetricsStorage(); // Default constructor that uses default Config
    ~MetricsStorage();

    // Initialize storage and create output directory
    bool initialize();

    // Add metrics samples (thread-safe)
    void addOSMetrics(const std::vector<OSMetrics>& metrics);
    void addGPUMetrics(const std::vector<GPUMetrics>& metrics);

    // Force flush current data to disk
    void flush();

    // Get statistics about stored data
    struct StorageStats {
        size_t total_os_samples = 0;
        size_t total_gpu_samples = 0;
        size_t os_files_written = 0;
        size_t gpu_files_written = 0;
        std::string last_os_file;
        std::string last_gpu_file;
    };
    StorageStats getStats() const;

private:
    // Internal file management
    struct FileWriter {
        std::unique_ptr<orc::Writer> writer;
        std::unique_ptr<orc::OutputStream> output;
        std::string filename;
        size_t row_count = 0;
        std::chrono::steady_clock::time_point created_at;
        std::unique_ptr<orc::Type> schema;
    };

    // OS metrics storage
    void createOSFile();
    void flushOSData();
    void addOSMetricsToBatch(const std::vector<OSMetrics>& metrics);

    // GPU metrics storage
    void createGPUFile();
    void flushGPUData();
    void addGPUMetricsToBatch(const std::vector<GPUMetrics>& metrics);

    // Common file management
    std::string generateFilename(const std::string& prefix, const std::string& extension = ".orc");
    bool shouldRollFile(const FileWriter& writer) const;
    void closeFile(FileWriter& writer);

    // ORC schema definitions
    std::unique_ptr<orc::Type> createOSSchema();
    std::unique_ptr<orc::Type> createGPUSchema();

    // Convert metrics to ORC batches
    void writeOSBatch(orc::Writer* writer, const std::vector<OSMetrics>& metrics);
    void writeGPUBatch(orc::Writer* writer, const std::vector<GPUMetrics>& metrics);

private:
    Config config_;

    // OS metrics storage - Fix: make mutexes mutable for const methods
    mutable std::mutex os_mutex_;
    std::unique_ptr<FileWriter> os_writer_;
    std::vector<OSMetrics> os_buffer_;
    StorageStats stats_;

    // GPU metrics storage
    mutable std::mutex gpu_mutex_;
    std::unique_ptr<FileWriter> gpu_writer_;
    std::vector<GPUMetrics> gpu_buffer_;
};

} // namespace unified_monitor