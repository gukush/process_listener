// os_metrics_linux.cpp
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <sys/types.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <limits>
#include <algorithm>

#include "simple_orchestrator.hpp"

namespace unified_monitor {

// ---------- helpers ----------

namespace {
struct CpuPrev { unsigned long long proc = 0; unsigned long long total = 0; bool init=false; };
struct IoPrev  { unsigned long long r = 0; unsigned long long w = 0; bool init=false; };
struct NetPrev { unsigned long long recv = 0; unsigned long long sent = 0; bool init=false; };

static std::unordered_map<pid_t, CpuPrev> g_cpu_prev;
static std::unordered_map<pid_t, IoPrev>  g_io_prev;
static NetPrev g_net_prev;
static std::mutex g_state_mtx;

static inline long get_num_cpus() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

static bool read_proc_status_mem(pid_t pid, long& rss_kb, long& vms_kb) {
    rss_kb = vms_kb = 0;
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            long kb=0; iss >> kb; rss_kb = kb;
        } else if (line.rfind("VmSize:", 0) == 0) {
            std::istringstream iss(line.substr(7));
            long kb=0; iss >> kb; vms_kb = kb;
        }
    }
    return (rss_kb>0 || vms_kb>0);
}

static void fallback_statm_mem(pid_t pid, long& rss_kb, long& vms_kb) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/statm");
    if (!f) return;
    unsigned long size_pages=0, resident_pages=0;
    f >> size_pages >> resident_pages;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    vms_kb = static_cast<long>(size_pages) * page_kb;
    rss_kb = static_cast<long>(resident_pages) * page_kb;
}

static bool read_proc_stat_times(pid_t pid, unsigned long long& utime, unsigned long long& stime) {
    utime = stime = 0;
    std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
    if (!f) return false;
    std::string line; std::getline(f, line);
    // Extract comm (which can contain spaces) between '(' and ')'
    auto lpar = line.find('(');
    auto rpar = line.rfind(')');
    if (lpar == std::string::npos || rpar == std::string::npos || rpar <= lpar) return false;
    std::string after = line.substr(rpar + 2); // skip ") "
    std::istringstream iss(after);
    // After comm: state,ppid,pgrp,session,tty_nr,tpgid,flags,minflt,cminflt,majflt,cmajflt,utime,stime,...
    for (int i = 0; i < 11; ++i) { unsigned long long tmp; if (!(iss >> tmp)) return false; }
    if (!(iss >> utime)) return false; // utime
    if (!(iss >> stime)) return false; // stime
    return true;
}

static bool read_total_jiffies(unsigned long long& total) {
    total = 0ULL;
    std::ifstream f("/proc/stat");
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    std::istringstream iss(line); // "cpu  <user> <nice> <system> <idle> ..."
    std::string cpu; iss >> cpu;
    unsigned long long v=0;
    while (iss >> v) total += v;
    return true;
}

static bool read_proc_io_bytes(pid_t pid, unsigned long long& rbytes, unsigned long long& wbytes) {
    rbytes = wbytes = 0ULL;
    std::ifstream f("/proc/" + std::to_string(pid) + "/io");
    if (!f) return false;
    std::string k; unsigned long long v;
    while (f >> k >> v) {
        if (k == "read_bytes:")       rbytes = v;
        else if (k == "write_bytes:") wbytes = v;
    }
    return true;
}

static bool read_net_totals(unsigned long long& recv, unsigned long long& sent) {
    recv = sent = 0ULL;
    std::ifstream f("/proc/net/dev");
    if (!f) return false;
    std::string line;
    std::getline(f, line); // header
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string ifname = line.substr(0, colon);
        ifname.erase(std::remove_if(ifname.begin(), ifname.end(), ::isspace), ifname.end());
        if (ifname == "lo") continue; // skip loopback

        std::string rest = line.substr(colon + 1);
        std::istringstream iss(rest);
        unsigned long long rx_bytes=0, tx_bytes=0;
        if (!(iss >> rx_bytes)) continue;         // 1) rx bytes
        for (int i=0;i<7;++i) { unsigned long long tmp; iss >> tmp; } // skip to tx
        if (!(iss >> tx_bytes)) continue;         // 9th value after colon
        recv += rx_bytes;
        sent += tx_bytes;
    }
    return true;
}

static inline unsigned long long saturating_delta(unsigned long long now, unsigned long long prev) {
    return (now >= prev) ? (now - prev) : 0ULL; // handle reset/wrap
}

} // namespace

OSMetrics OSMetricsCollector::collectForPid(pid_t pid) {
    OSMetrics m{};

    // Use wall clock for human-friendly timestamp
    const auto now_sys = std::chrono::system_clock::now();
    const auto ts_ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(now_sys)
                           .time_since_epoch().count();

    m.ts_unix_ns = static_cast<int64_t>(ts_ns);

    m.pid = pid;

    // -------- memory --------
    long rss_kb=0, vms_kb=0;
    if (!read_proc_status_mem(pid, rss_kb, vms_kb)) {
        fallback_statm_mem(pid, rss_kb, vms_kb);
    }
    m.mem_rss_kb = rss_kb;
    m.mem_vms_kb = vms_kb;

    // -------- cpu% --------
    {
        unsigned long long ut=0, st=0, total=0;
        double cpu_percent = 0.0;
        if (read_proc_stat_times(pid, ut, st) && read_total_jiffies(total)) {
            const unsigned long long proc = ut + st;

            std::lock_guard<std::mutex> lk(g_state_mtx);
            auto& prev = g_cpu_prev[pid];
            if (prev.init) {
                const unsigned long long dproc  = saturating_delta(proc, prev.proc);
                const unsigned long long dtotal = saturating_delta(total, prev.total);
                if (dtotal > 0ULL) {
                    cpu_percent = 100.0 * static_cast<double>(dproc) / static_cast<double>(dtotal);
                    cpu_percent *= static_cast<double>(get_num_cpus());
                    cpu_percent = std::clamp(cpu_percent, 0.0, 100.0 * static_cast<double>(get_num_cpus()));
                }
            }
            prev.proc = proc;
            prev.total = total;
            prev.init = true;
        }
        m.cpu_percent = cpu_percent;
    }

    // -------- disk I/O deltas --------
    {
        unsigned long long r=0, w=0;
        if (read_proc_io_bytes(pid, r, w)) {
            std::lock_guard<std::mutex> lk(g_state_mtx);
            auto& prev = g_io_prev[pid];
            unsigned long long dr = prev.init ? saturating_delta(r, prev.r) : 0ULL;
            unsigned long long dw = prev.init ? saturating_delta(w, prev.w) : 0ULL;
            prev.r = r; prev.w = w; prev.init = true;
            m.disk_read_bytes = dr;
            m.disk_write_bytes = dw;
        } else {
            m.disk_read_bytes = 0;
            m.disk_write_bytes = 0;
        }
    }

    // -------- network totals (system-wide, all non-loopback interfaces) --------
    {
        unsigned long long recv=0, sent=0;
        if (read_net_totals(recv, sent)) {
            std::lock_guard<std::mutex> lk(g_state_mtx);
            unsigned long long drecv = g_net_prev.init ? saturating_delta(recv, g_net_prev.recv) : 0ULL;
            unsigned long long dsent = g_net_prev.init ? saturating_delta(sent, g_net_prev.sent) : 0ULL;
            g_net_prev.recv = recv; g_net_prev.sent = sent; g_net_prev.init = true;
            m.net_recv_bytes = drecv;
            m.net_sent_bytes = dsent;
        } else {
            m.net_recv_bytes = 0;
            m.net_sent_bytes = 0;
        }
    }

    return m;
}

} // namespace unified_monitor
