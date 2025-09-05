// aggregation_export.cpp

class MetricsAggregator {
public:
    struct ChunkAggregates {
        std::string chunk_id;
        std::string task_id;
        double duration_ms;
        
        // OS metrics
        double avg_cpu_percent;
        double peak_cpu_percent;
        double total_cpu_time_ms;  // CPU time consumed
        double avg_memory_mb;
        double peak_memory_mb;
        double total_disk_read_mb;
        double total_disk_write_mb;
        double total_network_rx_mb;
        double total_network_tx_mb;
        
        // GPU metrics  
        double avg_gpu_util_percent;
        double peak_gpu_util_percent;
        double avg_gpu_memory_mb;
        double peak_gpu_memory_mb;
        double avg_power_watts;
        double peak_power_watts;
        double total_energy_joules;
        double avg_sm_clock_mhz;
        
        size_t sample_count;
    };
    
    struct TaskAggregates {
        std::string task_id;
        size_t chunk_count;
        double total_duration_ms;
        double total_compute_time_ms;  // Sum of all chunk durations
        
        // Aggregated from all chunks
        double total_cpu_time_ms;
        double peak_cpu_percent;
        double avg_memory_mb;
        double peak_memory_mb;
        double total_disk_io_mb;
        double total_network_io_mb;
        
        double total_energy_joules;
        double avg_gpu_util_percent;
        double peak_gpu_util_percent;
        
        // Efficiency metrics
        double parallelism_factor;  // total_compute_time / total_duration
        double gpu_efficiency;       // avg GPU util during execution
    };
    
    // Generate aggregates
    ChunkAggregates aggregateChunk(const ChunkMetrics& chunk);
    TaskAggregates aggregateTask(const std::string& task_id,
                                 const std::vector<ChunkMetrics>& chunks);
    
    // Export functions
    void exportChunkCSV(const std::string& filename,
                       const std::vector<ChunkAggregates>& chunks);
    void exportTaskCSV(const std::string& filename,
                      const std::vector<TaskAggregates>& tasks);
    void exportTimeSeriesCSV(const std::string& filename,
                           const ChunkMetrics& chunk);
};

TaskAggregates MetricsAggregator::aggregateTask(const std::string& task_id,
                                               const std::vector<ChunkMetrics>& chunks) {
    TaskAggregates task;
    task.task_id = task_id;
    task.chunk_count = chunks.size();
    
    if (chunks.empty()) return task;
    
    // Find overall task timeline
    auto task_start = chunks[0].arrival_time;
    auto task_end = chunks[0].completion_time;
    
    for (const auto& chunk : chunks) {
        task_start = std::min(task_start, chunk.arrival_time);
        task_end = std::max(task_end, chunk.completion_time);
        
        // Aggregate chunk metrics
        ChunkAggregates ca = aggregateChunk(chunk);
        
        task.total_compute_time_ms += ca.duration_ms;
        task.total_cpu_time_ms += ca.total_cpu_time_ms;
        task.peak_cpu_percent = std::max(task.peak_cpu_percent, ca.peak_cpu_percent);
        task.peak_memory_mb = std::max(task.peak_memory_mb, ca.peak_memory_mb);
        task.total_disk_io_mb += (ca.total_disk_read_mb + ca.total_disk_write_mb);
        task.total_network_io_mb += (ca.total_network_rx_mb + ca.total_network_tx_mb);
        task.total_energy_joules += ca.total_energy_joules;
        task.peak_gpu_util_percent = std::max(task.peak_gpu_util_percent, 
                                              ca.peak_gpu_util_percent);
    }
    
    task.total_duration_ms = std::chrono::duration<double, std::milli>(
        task_end - task_start).count();
    
    // Calculate efficiency metrics
    task.parallelism_factor = task.total_compute_time_ms / task.total_duration_ms;
    
    // Average memory across all chunks
    task.avg_memory_mb = 0;
    for (const auto& chunk : chunks) {
        ChunkAggregates ca = aggregateChunk(chunk);
        task.avg_memory_mb += ca.avg_memory_mb;
    }
    task.avg_memory_mb /= chunks.size();
    
    // GPU efficiency: weighted average by chunk duration
    double total_gpu_time = 0;
    for (const auto& chunk : chunks) {
        ChunkAggregates ca = aggregateChunk(chunk);
        task.avg_gpu_util_percent += ca.avg_gpu_util_percent * ca.duration_ms;
        total_gpu_time += ca.duration_ms;
    }
    if (total_gpu_time > 0) {
        task.avg_gpu_util_percent /= total_gpu_time;
        task.gpu_efficiency = task.avg_gpu_util_percent;
    }
    
    return task;
}

void MetricsAggregator::exportTaskCSV(const std::string& filename,
                                     const std::vector<TaskAggregates>& tasks) {
    std::ofstream file(filename);
    
    file << "task_id,chunk_count,total_duration_ms,total_compute_time_ms,"
         << "parallelism_factor,total_cpu_time_ms,peak_cpu_percent,"
         << "avg_memory_mb,peak_memory_mb,total_disk_io_mb,"
         << "total_network_io_mb,total_energy_joules,avg_gpu_util_percent,"
         << "peak_gpu_util_percent,gpu_efficiency\n";
    
    for (const auto& task : tasks) {
        file << task.task_id << ","
             << task.chunk_count << ","
             << task.total_duration_ms << ","
             << task.total_compute_time_ms << ","
             << task.parallelism_factor << ","
             << task.total_cpu_time_ms << ","
             << task.peak_cpu_percent << ","
             << task.avg_memory_mb << ","
             << task.peak_memory_mb << ","
             << task.total_disk_io_mb << ","
             << task.total_network_io_mb << ","
             << task.total_energy_joules << ","
             << task.avg_gpu_util_percent << ","
             << task.peak_gpu_util_percent << ","
             << task.gpu_efficiency << "\n";
    }
}
