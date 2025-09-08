#include "metrics_storage.hpp"
#include "orchestrator.hpp"
#include <arrow/builder.h>
#include <arrow/type.h>
#include <arrow/array.h>
#include <arrow/table.h>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace unified_monitor {

MetricsStorage::MetricsStorage(const Config& config) : config_(config) {
    os_buffer_.reserve(1000);  // Pre-allocate buffer
    gpu_buffer_.reserve(1000);
}

MetricsStorage::~MetricsStorage() {
    flush();
}

bool MetricsStorage::initialize() {
    try {
        std::filesystem::create_directories(config_.output_dir);
        std::cout << "[MetricsStorage] Initialized with output dir: " << config_.output_dir << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] Failed to create output directory: " << e.what() << std::endl;
        return false;
    }
}

void MetricsStorage::addOSMetrics(const std::vector<OSMetrics>& metrics) {
    if (metrics.empty()) return;

    std::lock_guard<std::mutex> lock(os_mutex_);
    os_buffer_.insert(os_buffer_.end(), metrics.begin(), metrics.end());

    // Flush if buffer is getting large
    if (os_buffer_.size() >= 1000) {
        flushOSData();
    }
}

void MetricsStorage::addGPUMetrics(const std::vector<GPUMetrics>& metrics) {
#if HAVE_CUDA
    if (metrics.empty()) return;

    std::lock_guard<std::mutex> lock(gpu_mutex_);
    gpu_buffer_.insert(gpu_buffer_.end(), metrics.begin(), metrics.end());

    // Flush if buffer is getting large
    if (gpu_buffer_.size() >= 1000) {
        flushGPUData();
    }
#else
    (void)metrics; // Suppress unused parameter warning
#endif
}

void MetricsStorage::flush() {
    std::lock_guard<std::mutex> os_lock(os_mutex_);
    std::lock_guard<std::mutex> gpu_lock(gpu_mutex_);

    flushOSData();
#if HAVE_CUDA
    flushGPUData();
#endif
}

MetricsStorage::StorageStats MetricsStorage::getStats() const {
    std::lock_guard<std::mutex> os_lock(os_mutex_);
    std::lock_guard<std::mutex> gpu_lock(gpu_mutex_);
    return stats_;
}

void MetricsStorage::createOSFile() {
    if (os_writer_) {
        closeFile(*os_writer_);
    }

    auto filename = generateFilename("os_metrics");
    auto output = std::make_unique<arrow::io::FileOutputStream>(filename);
    if (!output->Open().ok()) {
        std::cerr << "[MetricsStorage] Failed to create OS file: " << filename << std::endl;
        return;
    }

    auto schema = createOSSchema();
    if (!schema) {
        std::cerr << "[MetricsStorage] Failed to create OS schema" << std::endl;
        return;
    }

    // Create ORC writer options
    orc::WriterOptions options;
    options.setCompression(orc::CompressionKind_ZSTD);
    options.setCompressionStrategy(orc::CompressionStrategy_SPEED);
    options.setCompressionBlockSize(64 * 1024); // 64KB blocks
    options.setStripeSize(64 * 1024 * 1024);   // 64MB stripes
    options.setRowIndexStride(10000);          // Row index every 10k rows

    try {
        auto writer = orc::createWriter(*schema, output.get(), options);

        os_writer_ = std::make_unique<FileWriter>();
        os_writer_->writer = std::move(writer);
        os_writer_->output = std::move(output);
        os_writer_->filename = filename;
        os_writer_->created_at = std::chrono::steady_clock::now();
        os_writer_->schema = std::move(schema);

        stats_.last_os_file = filename;
        std::cout << "[MetricsStorage] Created OS file: " << filename << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] Failed to create OS ORC writer: " << e.what() << std::endl;
    }
}

void MetricsStorage::flushOSData() {
    if (os_buffer_.empty()) return;

    if (!os_writer_ || shouldRollFile(*os_writer_)) {
        createOSFile();
    }

    if (!os_writer_ || !os_writer_->writer) {
        std::cerr << "[MetricsStorage] No valid OS writer available" << std::endl;
        return;
    }

    try {
        writeOSBatch(os_writer_->writer.get(), os_buffer_);
        os_writer_->row_count += os_buffer_.size();
        stats_.total_os_samples += os_buffer_.size();
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] Failed to write OS batch: " << e.what() << std::endl;
    }

    os_buffer_.clear();
}

void MetricsStorage::createGPUFile() {
#if HAVE_CUDA
    if (gpu_writer_) {
        closeFile(*gpu_writer_);
    }

    auto filename = generateFilename("gpu_metrics");
    auto output = std::make_unique<arrow::io::FileOutputStream>(filename);
    if (!output->Open().ok()) {
        std::cerr << "[MetricsStorage] Failed to create GPU file: " << filename << std::endl;
        return;
    }

    auto schema = createGPUSchema();
    if (!schema) {
        std::cerr << "[MetricsStorage] Failed to create GPU schema" << std::endl;
        return;
    }

    // Create ORC writer options
    orc::WriterOptions options;
    options.setCompression(orc::CompressionKind_ZSTD);
    options.setCompressionStrategy(orc::CompressionStrategy_SPEED);
    options.setCompressionBlockSize(64 * 1024); // 64KB blocks
    options.setStripeSize(64 * 1024 * 1024);   // 64MB stripes
    options.setRowIndexStride(10000);          // Row index every 10k rows

    try {
        auto writer = orc::createWriter(*schema, output.get(), options);

        gpu_writer_ = std::make_unique<FileWriter>();
        gpu_writer_->writer = std::move(writer);
        gpu_writer_->output = std::move(output);
        gpu_writer_->filename = filename;
        gpu_writer_->created_at = std::chrono::steady_clock::now();
        gpu_writer_->schema = std::move(schema);

        stats_.last_gpu_file = filename;
        std::cout << "[MetricsStorage] Created GPU file: " << filename << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] Failed to create GPU ORC writer: " << e.what() << std::endl;
    }
#else
    // GPU metrics disabled
#endif
}

void MetricsStorage::flushGPUData() {
#if HAVE_CUDA
    if (gpu_buffer_.empty()) return;

    if (!gpu_writer_ || shouldRollFile(*gpu_writer_)) {
        createGPUFile();
    }

    if (!gpu_writer_ || !gpu_writer_->writer) {
        std::cerr << "[MetricsStorage] No valid GPU writer available" << std::endl;
        return;
    }

    try {
        writeGPUBatch(gpu_writer_->writer.get(), gpu_buffer_);
        gpu_writer_->row_count += gpu_buffer_.size();
        stats_.total_gpu_samples += gpu_buffer_.size();
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] Failed to write GPU batch: " << e.what() << std::endl;
    }

    gpu_buffer_.clear();
#else
    // GPU metrics disabled
#endif
}

std::string MetricsStorage::generateFilename(const std::string& prefix, const std::string& extension) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << config_.output_dir << "/" << prefix << "_"
       << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S")
       << "_" << std::setfill('0') << std::setw(3) << ms.count()
       << extension;
    return ss.str();
}

bool MetricsStorage::shouldRollFile(const FileWriter& writer) const {
    if (writer.row_count >= config_.max_rows_per_file) return true;

    auto now = std::chrono::steady_clock::now();
    auto age = now - writer.created_at;
    return age >= config_.max_file_age;
}

void MetricsStorage::closeFile(FileWriter& writer) {
    if (writer.writer) {
        writer.writer->close();
        writer.writer.reset();
    }
    if (writer.output) {
        writer.output->Close();
        writer.output.reset();
    }
}

std::unique_ptr<orc::Type> MetricsStorage::createOSSchema() {
    try {
        return orc::createStructType()
            ->addField("timestamp", orc::createPrimitiveType(orc::DOUBLE))
            ->addField("pid", orc::createPrimitiveType(orc::INT))
            ->addField("cpu_percent", orc::createPrimitiveType(orc::FLOAT))
            ->addField("mem_rss_kb", orc::createPrimitiveType(orc::LONG))
            ->addField("mem_vms_kb", orc::createPrimitiveType(orc::LONG))
            ->addField("disk_read_bytes", orc::createPrimitiveType(orc::LONG))
            ->addField("disk_write_bytes", orc::createPrimitiveType(orc::LONG))
            ->addField("net_recv_bytes", orc::createPrimitiveType(orc::LONG))
            ->addField("net_sent_bytes", orc::createPrimitiveType(orc::LONG));
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] Failed to create OS schema: " << e.what() << std::endl;
        return nullptr;
    }
}

std::unique_ptr<orc::Type> MetricsStorage::createGPUSchema() {
#if HAVE_CUDA
    try {
        return orc::createStructType()
            ->addField("timestamp", orc::createPrimitiveType(orc::DOUBLE))
            ->addField("gpu_index", orc::createPrimitiveType(orc::INT))
            ->addField("power_mw", orc::createPrimitiveType(orc::INT))
            ->addField("gpu_util_percent", orc::createPrimitiveType(orc::INT))
            ->addField("mem_util_percent", orc::createPrimitiveType(orc::INT))
            ->addField("mem_used_bytes", orc::createPrimitiveType(orc::LONG))
            ->addField("sm_clock_mhz", orc::createPrimitiveType(orc::INT));
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] Failed to create GPU schema: " << e.what() << std::endl;
        return nullptr;
    }
#else
    return nullptr; // GPU metrics disabled
#endif
}

void MetricsStorage::writeOSBatch(orc::Writer* writer, const std::vector<OSMetrics>& metrics) {
    if (metrics.empty()) return;

    try {
        auto batch = writer->createRowBatch(static_cast<uint64_t>(metrics.size()));
        auto& structBatch = dynamic_cast<orc::StructVectorBatch&>(*batch);

        // Get column vectors
        auto& timestampCol = dynamic_cast<orc::DoubleVectorBatch&>(*structBatch.fields[0]);
        auto& pidCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[1]);
        auto& cpuCol = dynamic_cast<orc::DoubleVectorBatch&>(*structBatch.fields[2]);
        auto& memRssCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[3]);
        auto& memVmsCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[4]);
        auto& diskReadCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[5]);
        auto& diskWriteCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[6]);
        auto& netRecvCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[7]);
        auto& netSentCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[8]);

        // Fill data
        for (size_t i = 0; i < metrics.size(); ++i) {
            const auto& m = metrics[i];
            timestampCol.data[i] = m.timestamp;
            pidCol.data[i] = static_cast<int64_t>(m.pid);
            cpuCol.data[i] = static_cast<double>(m.cpu_percent);
            memRssCol.data[i] = static_cast<int64_t>(m.mem_rss_kb);
            memVmsCol.data[i] = static_cast<int64_t>(m.mem_vms_kb);
            diskReadCol.data[i] = static_cast<int64_t>(m.disk_read_bytes);
            diskWriteCol.data[i] = static_cast<int64_t>(m.disk_write_bytes);
            netRecvCol.data[i] = static_cast<int64_t>(m.net_recv_bytes);
            netSentCol.data[i] = static_cast<int64_t>(m.net_sent_bytes);
        }

        // Set null indicators (all non-null for now)
        structBatch.numElements = static_cast<uint64_t>(metrics.size());
        structBatch.hasNulls = false;

        for (int i = 0; i < 9; ++i) {
            structBatch.fields[i]->numElements = static_cast<uint64_t>(metrics.size());
            structBatch.fields[i]->hasNulls = false;
        }

        writer->add(*batch);
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] Failed to write OS batch: " << e.what() << std::endl;
        throw;
    }
}

void MetricsStorage::writeGPUBatch(orc::Writer* writer, const std::vector<GPUMetrics>& metrics) {
#if HAVE_CUDA
    if (metrics.empty()) return;

    try {
        auto batch = writer->createRowBatch(static_cast<uint64_t>(metrics.size()));
        auto& structBatch = dynamic_cast<orc::StructVectorBatch&>(*batch);

        // Get column vectors
        auto& timestampCol = dynamic_cast<orc::DoubleVectorBatch&>(*structBatch.fields[0]);
        auto& gpuIndexCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[1]);
        auto& powerCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[2]);
        auto& gpuUtilCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[3]);
        auto& memUtilCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[4]);
        auto& memUsedCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[5]);
        auto& smClockCol = dynamic_cast<orc::LongVectorBatch&>(*structBatch.fields[6]);

        // Fill data
        for (size_t i = 0; i < metrics.size(); ++i) {
            const auto& m = metrics[i];
            timestampCol.data[i] = m.timestamp;
            gpuIndexCol.data[i] = static_cast<int64_t>(m.gpu_index);
            powerCol.data[i] = static_cast<int64_t>(m.power_mw);
            gpuUtilCol.data[i] = static_cast<int64_t>(m.gpu_util_percent);
            memUtilCol.data[i] = static_cast<int64_t>(m.mem_util_percent);
            memUsedCol.data[i] = static_cast<int64_t>(m.mem_used_bytes);
            smClockCol.data[i] = static_cast<int64_t>(m.sm_clock_mhz);
        }

        // Set null indicators (all non-null for now)
        structBatch.numElements = static_cast<uint64_t>(metrics.size());
        structBatch.hasNulls = false;

        for (int i = 0; i < 7; ++i) {
            structBatch.fields[i]->numElements = static_cast<uint64_t>(metrics.size());
            structBatch.fields[i]->hasNulls = false;
        }

        writer->add(*batch);
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] Failed to write GPU batch: " << e.what() << std::endl;
        throw;
    }
#else
    (void)writer; (void)metrics; // Suppress unused parameter warnings
#endif
}

} // namespace unified_monitor