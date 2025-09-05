// os_metrics_linux.cpp
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>

#include "orchestrator.hpp"

namespace unified_monitor {

OSMetrics OSMetricsCollector::collectForPid(pid_t pid) {
    OSMetrics m{};
    m.timestamp = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    m.pid = pid;

    // Read RSS and VMS from /proc/[pid]/status
    {
        std::ifstream f("/proc/" + std::to_string(pid) + "/status");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream iss(line.substr(6));
                long kb = 0; std::string unit;
                iss >> kb >> unit;
                m.mem_rss_kb = kb;
            } else if (line.rfind("VmSize:", 0) == 0) {
                std::istringstream iss(line.substr(7));
                long kb = 0; std::string unit;
                iss >> kb >> unit;
                m.mem_vms_kb = kb;
            }
        }
    }

    // Disk/network deltas left 0 in this stub (per-sample deltas need state).
    m.disk_read_bytes = 0;
    m.disk_write_bytes = 0;
    m.net_recv_bytes = 0;
    m.net_sent_bytes = 0;

    // CPU percentage calculation needs previous sample; keep 0.0 in this stub.
    m.cpu_percent = 0.0;

    return m;
}

} // namespace unified_monitor
