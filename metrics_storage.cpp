#include "metrics_storage.hpp"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <ctime>

namespace unified_monitor {

namespace {

std::string sanitizeLabel(const std::string& label) {
    std::string out;
    out.reserve(label.size());
    for (unsigned char ch : label) {
        if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "unknown" : out;
}

} // namespace

MetricsStorage::MetricsStorage(const Config& config) : config_(config) {
    buffer_.reserve(4096);
}

MetricsStorage::MetricsStorage() : MetricsStorage(Config{}) {}

MetricsStorage::~MetricsStorage() {
    flushAndClose();
}

void MetricsStorage::setStorageConfig(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

bool MetricsStorage::initialize() {
    try {
        std::filesystem::create_directories(config_.output_dir);
        createFile();
        return file_ && file_->writer;
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] initialize failed: " << e.what() << std::endl;
        return false;
    }
}

void MetricsStorage::addSamples(const std::vector<UnifiedMetricSample>& samples) {
    if (samples.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.insert(buffer_.end(), samples.begin(), samples.end());
    if (buffer_.size() >= 1024) {
        flushData();
    }
}

void MetricsStorage::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    flushData();
}

void MetricsStorage::flushAndClose() {
    if (closed_.exchange(true)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    flushData();
    closeFile();
}

MetricsStorage::StorageStats MetricsStorage::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void MetricsStorage::createFile() {
    if (file_) return;

    auto schema = createSchema();
    auto filename = generateFilename();
    auto output = orc::writeLocalFile(filename);

    orc::WriterOptions options;
    options.setCompression(config_.use_zstd_compression ? orc::CompressionKind_ZSTD
                                                        : orc::CompressionKind_NONE);
    options.setCompressionStrategy(orc::CompressionStrategy_SPEED);
    options.setCompressionBlockSize(64 * 1024);
    options.setStripeSize(64 * 1024 * 1024);
    options.setRowIndexStride(10000);

    auto writer = orc::createWriter(*schema, output.get(), options);

    file_ = std::make_unique<FileWriter>();
    file_->writer = std::move(writer);
    file_->output = std::move(output);
    file_->schema = std::move(schema);
    file_->filename = filename;
    stats_.filename = filename;

    std::cout << "[MetricsStorage] Writing " << filename << std::endl;
}

void MetricsStorage::flushData() {
    if (buffer_.empty()) return;
    if (!file_ || !file_->writer) {
        createFile();
    }

    writeBatch(file_->writer.get(), buffer_);
    file_->row_count += buffer_.size();
    stats_.total_samples += buffer_.size();
    buffer_.clear();
}

void MetricsStorage::closeFile() {
    if (!file_) return;

    try {
        if (file_->writer) {
            file_->writer->close();
            file_->writer.reset();
        }
        file_->output.reset();
        stats_.files_written = 1;
        std::cout << "[MetricsStorage] Closed " << file_->filename
                  << " (" << file_->row_count << " rows)" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[MetricsStorage] close failed: " << e.what() << std::endl;
    }
}

std::string MetricsStorage::generateFilename() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::tm tm{};
    localtime_r(&time_t, &tm);

    std::ostringstream ss;
    ss << config_.output_dir << "/metrics_" << sanitizeLabel(config_.label) << "_"
       << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_"
       << std::setfill('0') << std::setw(3) << ms.count()
       << ".orc";
    return ss.str();
}

std::unique_ptr<orc::Type> MetricsStorage::createSchema() const {
    return std::unique_ptr<orc::Type>(
        orc::Type::buildTypeFromString(
            "struct<ts_unix_ns:bigint,sample_index:bigint,label:string,"
            "gpu_index:int,gpu_power_mw:bigint,rapl_zone_index:int,rapl_zone_name:string,"
            "cpu_energy_delta_uj:bigint,cpu_energy_total_uj:bigint>"
        )
    );
}

void MetricsStorage::writeBatch(orc::Writer* writer, const std::vector<UnifiedMetricSample>& samples) {
    auto batch = writer->createRowBatch(static_cast<uint64_t>(samples.size()));
    auto& root = dynamic_cast<orc::StructVectorBatch&>(*batch);

    auto& ts_col = dynamic_cast<orc::LongVectorBatch&>(*root.fields[0]);
    auto& sample_col = dynamic_cast<orc::LongVectorBatch&>(*root.fields[1]);
    auto& label_col = dynamic_cast<orc::StringVectorBatch&>(*root.fields[2]);
    auto& gpu_index_col = dynamic_cast<orc::LongVectorBatch&>(*root.fields[3]);
    auto& gpu_power_col = dynamic_cast<orc::LongVectorBatch&>(*root.fields[4]);
    auto& zone_index_col = dynamic_cast<orc::LongVectorBatch&>(*root.fields[5]);
    auto& zone_name_col = dynamic_cast<orc::StringVectorBatch&>(*root.fields[6]);
    auto& energy_delta_col = dynamic_cast<orc::LongVectorBatch&>(*root.fields[7]);
    auto& energy_total_col = dynamic_cast<orc::LongVectorBatch&>(*root.fields[8]);

    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& s = samples[i];
        ts_col.data[i] = s.ts_unix_ns;
        sample_col.data[i] = static_cast<int64_t>(s.sample_index);
        label_col.data[i] = const_cast<char*>(s.label.c_str());
        label_col.length[i] = static_cast<int64_t>(s.label.size());
        gpu_index_col.data[i] = static_cast<int64_t>(s.gpu_index);
        gpu_power_col.data[i] = s.gpu_power_mw;
        zone_index_col.data[i] = static_cast<int64_t>(s.rapl_zone_index);
        zone_name_col.data[i] = const_cast<char*>(s.rapl_zone_name.c_str());
        zone_name_col.length[i] = static_cast<int64_t>(s.rapl_zone_name.size());
        energy_delta_col.data[i] = static_cast<int64_t>(s.cpu_energy_delta_uj);
        energy_total_col.data[i] = static_cast<int64_t>(s.cpu_energy_total_uj);
    }

    root.numElements = static_cast<uint64_t>(samples.size());
    root.hasNulls = false;
    for (size_t i = 0; i < root.fields.size(); ++i) {
        root.fields[i]->numElements = static_cast<uint64_t>(samples.size());
        root.fields[i]->hasNulls = false;
    }

    writer->add(*batch);
}

} // namespace unified_monitor
