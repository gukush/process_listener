// csv_export.cpp

void ChunkTracker::exportToCSV(const std::string& filename) const {
    std::ofstream file(filename);
    
    // Header
    file << "chunk_id,task_id,arrival_time,start_time,completion_time,"
         << "duration_ms,status,energy_joules,avg_cpu_percent,"
         << "peak_cpu_percent,avg_mem_mb,peak_mem_mb,avg_gpu_util,"
         << "peak_gpu_util,avg_gpu_power_w,total_disk_read_mb,"
         << "total_disk_write_mb,total_net_recv_mb,total_net_sent_mb\n";
    
    for (const auto& [chunk_id, metrics] : chunks_) {
        double duration_ms = std::chrono::duration<double, std::milli>(
            metrics.completion_time - metrics.arrival_time).count();
        
        // Calculate aggregates
        double avg_cpu = 0, peak_cpu = 0;
        double avg_mem_mb = 0, peak_mem_mb = 0;
        double avg_gpu_util = 0, peak_gpu_util = 0;
        double avg_gpu_power_w = 0;
        
        // Process OS metrics
        for (const auto& os : metrics.os_samples) {
            avg_cpu += os.cpu_percent;
            peak_cpu = std::max(peak_cpu, os.cpu_percent);
            
            double mem_mb = os.mem_rss_kb / 1024.0;
            avg_mem_mb += mem_mb;
            peak_mem_mb = std::max(peak_mem_mb, mem_mb);
        }
        
        if (!metrics.os_samples.empty()) {
            avg_cpu /= metrics.os_samples.size();
            avg_mem_mb /= metrics.os_samples.size();
        }
        
        // Process GPU metrics
        for (const auto& gpu : metrics.gpu_samples) {
            avg_gpu_util += gpu.gpu_util_percent;
            peak_gpu_util = std::max(peak_gpu_util, 
                                     (double)gpu.gpu_util_percent);
            avg_gpu_power_w += gpu.power_mw / 1000.0;
        }
        
        if (!metrics.gpu_samples.empty()) {
            avg_gpu_util /= metrics.gpu_samples.size();
            avg_gpu_power_w /= metrics.gpu_samples.size();
        }
        
        // Calculate totals
        double total_disk_read_mb = 0, total_disk_write_mb = 0;
        double total_net_recv_mb = 0, total_net_sent_mb = 0;
        
        if (!metrics.os_samples.empty()) {
            const auto& last = metrics.os_samples.back();
            const auto& first = metrics.os_samples.front();
            
            total_disk_read_mb = (last.disk_read_bytes - 
                                 first.disk_read_bytes) / (1024.0 * 1024.0);
            total_disk_write_mb = (last.disk_write_bytes - 
                                  first.disk_write_bytes) / (1024.0 * 1024.0);
            total_net_recv_mb = (last.net_recv_bytes - 
                                first.net_recv_bytes) / (1024.0 * 1024.0);
            total_net_sent_mb = (last.net_sent_bytes - 
                                first.net_sent_bytes) / (1024.0 * 1024.0);
        }
        
        // Write row
        file << chunk_id << ","
             << metrics.task_id << ","
             << std::chrono::duration<double>(
                    metrics.arrival_time.time_since_epoch()).count() << ","
             << std::chrono::duration<double>(
                    metrics.start_time.time_since_epoch()).count() << ","
             << std::chrono::duration<double>(
                    metrics.completion_time.time_since_epoch()).count() << ","
             << duration_ms << ","
             << metrics.status << ","
             << metrics.energy_joules << ","
             << avg_cpu << ","
             << peak_cpu << ","
             << avg_mem_mb << ","
             << peak_mem_mb << ","
             << avg_gpu_util << ","
             << peak_gpu_util << ","
             << avg_gpu_power_w << ","
             << total_disk_read_mb << ","
             << total_disk_write_mb << ","
             << total_net_recv_mb << ","
             << total_net_sent_mb << "\n";
    }
    
    file.close();
}
