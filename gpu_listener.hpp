#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>
#include <nvml.h>

namespace gpu_listener {

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;
namespace beast = boost::beast;
using json = nlohmann::json;

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using ms = std::chrono::milliseconds;

struct NvmlSample {
    double ts_sec{0};
    unsigned int power_mw{0};
    unsigned int util_gpu{0};
    unsigned int util_mem{0};
    unsigned long long mem_used_bytes{0};
    unsigned int clocks_sm_mhz{0};
    std::map<unsigned int, unsigned int> pid_gpu_percent; // pid -> %
};

struct NvmlSummary {
    // Core
    double duration_sec{0};
    double energy_joules{0};
    double energy_wh{0};
    double avg_power_w{0};

    // Peaks
    unsigned int peak_util_gpu{0};
    unsigned int peak_util_mem{0};
    unsigned long long peak_mem_used_bytes{0};
    unsigned int peak_clocks_sm_mhz{0};

    // Averages & p95
    double avg_gpu_util{0};
    double p95_gpu_util{0};
    double avg_mem_util{0};
    double p95_mem_util{0};
    double avg_mem_used_bytes{0};
    double avg_sm_clock_mhz{0};

    // Per-PID
    std::map<unsigned int, double> pid_avg_gpu_percent; // %
    std::map<unsigned int, double> pid_energy_joules;   // estimated if per-PID util available
    bool pid_energy_estimated{false};

    // Misc
    size_t samples_count{0};
};

class NvmlDevice {
public:
    explicit NvmlDevice(unsigned index);
    nvmlDevice_t handle() const { return handle_; }

    bool getPowerMilliwatts(unsigned int& mw) const;
    bool getUtilization(unsigned int& gpu, unsigned int& mem) const;
    bool getMemoryUsed(unsigned long long& used) const;
    bool getSmClock(unsigned int& mhz) const;

    // Try per-PID SM% if available; fallback to just PIDs with 0%
    std::map<unsigned int, unsigned int> getPerPidGpuPercent(unsigned long long last_seen_usec) const;

    static std::string errorString(nvmlReturn_t st);

private:
    nvmlDevice_t handle_{};
};

class MonitorSession {
public:
    std::string chunk_id;
    std::optional<unsigned int> hinted_pid;
    unsigned gpu_index{0};

    // control
    std::atomic<bool> running{false};
    std::thread worker;
    ms interval{ms(50)}; // ~20Hz

    // timing
    TimePoint t0{};
    TimePoint t_last{};

    // data
    std::vector<NvmlSample> samples;

    // live accumulators
    unsigned int peak_util_gpu{0};
    unsigned int peak_util_mem{0};
    unsigned long long peak_mem_used_bytes{0};
    unsigned int peak_clocks_sm_mhz{0};
    double energy_joules{0.0};
    unsigned int last_power_mw{0};

    // series for avg/p95
    std::vector<unsigned int> series_util_gpu;
    std::vector<unsigned int> series_util_mem;
    std::vector<unsigned long long> series_mem_used;
    std::vector<unsigned int> series_sm_clock;

    // per-PID energy
    std::map<unsigned int, double> pid_energy_j;
    bool pid_energy_possible{false};

    void start(unsigned gpuIdx);
    NvmlSummary stopAndSummarize();
    json summaryToJson(const NvmlSummary& s) const;

    // Logging
    static void ensure_logs_dir();
    static void append_csv_header_if_needed(const std::filesystem::path& fpath);
    static std::string now_iso8601();
    void write_chunk_logs(const NvmlSummary& s, const json& as_json, bool write_samples_csv=false) const;
};

class MonitorManager {
public:
    static MonitorManager& instance();
    std::shared_ptr<MonitorSession> start(const std::string& chunk_id, unsigned gpu_index, std::optional<unsigned> pid_hint);
    json end(const std::string& chunk_id);
    void stopAll();
private:
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<MonitorSession>> sessions_;
};

// WebSocket session & listener
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    explicit WsSession(tcp::socket socket);
    void run();
private:
    websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    std::deque<std::string> send_queue_;

    void onAccept(beast::error_code ec);
    void doRead();
    void onRead(beast::error_code ec, std::size_t);
    void write(const json& j);
    void doWrite();
    void onWrite(beast::error_code ec, std::size_t);
    static json errorMsg(const std::string& code, const std::string& msg);
    void handleMessage(const json& obj);

    static void fail(beast::error_code ec, const char* what);
};

class WsListener : public std::enable_shared_from_this<WsListener> {
public:
    WsListener(boost::asio::io_context& ioc, tcp::endpoint ep);
    void run();
private:
    boost::asio::io_context& ioc_;
    tcp::acceptor acceptor_;
    void doAccept();
    void onAccept(beast::error_code ec, tcp::socket socket);
    static void fail(beast::error_code ec, const char* what);
};

// Entrypoint helper (optional)
int run_server(unsigned short port = 8765, const std::string& host = "127.0.0.1");

} // namespace gpu_listener
