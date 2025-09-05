// os_metrics_linux.cpp
#include <fstream>
#include <sstream>
#include <unistd.h>

OSMetrics OSMetricsCollector::collectForPid(pid_t pid) {
    OSMetrics metrics;
    metrics.pid = pid;
    metrics.timestamp = std::chrono::duration<double>(
        Clock::now().time_since_epoch()).count();
    
    // CPU usage from /proc/[pid]/stat
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    if (stat_file.is_open()) {
        std::string line;
        std::getline(stat_file, line);
        std::istringstream iss(line);
        
        // Skip to utime and stime fields (14th and 15th)
        std::string dummy;
        for (int i = 0; i < 13; i++) iss >> dummy;
        
        unsigned long utime, stime;
        iss >> utime >> stime;
        
        // Calculate CPU percentage
        auto& state = process_states_[pid];
        if (state.prev_utime > 0) {
            auto now = Clock::now();
            double time_diff = std::chrono::duration<double>(
                now - state.last_update).count();
            
            unsigned long cpu_ticks = (utime - state.prev_utime) + 
                                      (stime - state.prev_stime);
            metrics.cpu_percent = (cpu_ticks / sysconf(_SC_CLK_TCK)) / 
                                  time_diff * 100.0;
        }
        
        state.prev_utime = utime;
        state.prev_stime = stime;
        state.last_update = Clock::now();
    }
    
    // Memory from /proc/[pid]/status
    std::ifstream status_file("/proc/" + std::to_string(pid) + "/status");
    if (status_file.is_open()) {
        std::string line;
        while (std::getline(status_file, line)) {
            if (line.find("VmRSS:") == 0) {
                std::istringstream iss(line);
                std::string label;
                iss >> label >> metrics.mem_rss_kb;
            } else if (line.find("VmSize:") == 0) {
                std::istringstream iss(line);
                std::string label;
                iss >> label >> metrics.mem_vms_kb;
            }
        }
    }
    
    // Disk I/O from /proc/[pid]/io
    std::ifstream io_file("/proc/" + std::to_string(pid) + "/io");
    if (io_file.is_open()) {
        std::string line;
        while (std::getline(io_file, line)) {
            if (line.find("read_bytes:") == 0) {
                sscanf(line.c_str(), "read_bytes: %lu", &metrics.disk_read_bytes);
            } else if (line.find("write_bytes:") == 0) {
                sscanf(line.c_str(), "write_bytes: %lu", &metrics.disk_write_bytes);
            }
        }
    }
    
    // Network stats from /proc/[pid]/net/dev (simplified)
    std::ifstream net_file("/proc/" + std::to_string(pid) + "/net/dev");
    if (net_file.is_open()) {
        std::string line;
        unsigned long total_recv = 0, total_sent = 0;
        
        // Skip header lines
        std::getline(net_file, line);
        std::getline(net_file, line);
        
        while (std::getline(net_file, line)) {
            if (line.find("eth") != std::string::npos || 
                line.find("wlan") != std::string::npos) {
                unsigned long recv, sent;
                sscanf(line.c_str(), "%*s %lu %*lu %*lu %*lu %*lu %*lu %*lu %*lu %lu",
                       &recv, &sent);
                total_recv += recv;
                total_sent += sent;
            }
        }
        
        metrics.net_recv_bytes = total_recv;
        metrics.net_sent_bytes = total_sent;
    }
    
    return metrics;
}
