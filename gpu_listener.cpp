#include "gpu_listener.hpp"

#include <algorithm>
#include <csignal>
#include <iomanip>

namespace gpu_listener {

static std::string nvmlErrorStr(nvmlReturn_t st) {
    const char* s = nvmlErrorString(st);
    return s ? std::string(s) : "Unknown NVML error";
}

std::string NvmlDevice::errorString(nvmlReturn_t st) { return nvmlErrorStr(st); }

// -------------------- NvmlDevice --------------------

NvmlDevice::NvmlDevice(unsigned index) {
    nvmlReturn_t st = nvmlInit_v2();
    if (st != NVML_SUCCESS && st != NVML_ERROR_ALREADY_INITIALIZED) {
        throw std::runtime_error("NVML init failed: " + nvmlErrorStr(st));
    }
    st = nvmlDeviceGetHandleByIndex_v2(index, &handle_);
    if (st != NVML_SUCCESS) {
        throw std::runtime_error("nvmlDeviceGetHandleByIndex_v2 failed: " + nvmlErrorStr(st));
    }
}

bool NvmlDevice::getPowerMilliwatts(unsigned int& mw) const {
    auto st = nvmlDeviceGetPowerUsage(handle_, &mw);
    return st == NVML_SUCCESS;
}
bool NvmlDevice::getUtilization(unsigned int& gpu, unsigned int& mem) const {
    nvmlUtilizationRates_t r{};
    auto st = nvmlDeviceGetUtilizationRates(handle_, &r);
    if (st != NVML_SUCCESS) return false;
    gpu = r.gpu; mem = r.memory; return true;
}
bool NvmlDevice::getMemoryUsed(unsigned long long& used) const {
    nvmlMemory_t m{};
    auto st = nvmlDeviceGetMemoryInfo(handle_, &m);
    if (st != NVML_SUCCESS) return false;
    used = m.used; return true;
}
bool NvmlDevice::getSmClock(unsigned int& mhz) const {
    auto st = nvmlDeviceGetClockInfo(handle_, NVML_CLOCK_SM, &mhz);
    return st == NVML_SUCCESS;
}

std::map<unsigned int, unsigned int>
NvmlDevice::getPerPidGpuPercent(unsigned long long last_seen_usec) const {
    std::map<unsigned int, unsigned int> out;

    // Try newer API
    {
        const unsigned int MAX_SAMPLES = 1024;
        std::vector<nvmlProcessUtilizationSample_t> samples(MAX_SAMPLES);
        unsigned int n = MAX_SAMPLES;
        auto st = nvmlDeviceGetProcessUtilization(handle_, samples.data(), &n, last_seen_usec);
        if (st == NVML_SUCCESS) {
            for (unsigned i=0;i<n;i++) out[samples[i].pid] = samples[i].smUtil;
            return out;
        }
        // else: NOT_SUPPORTED or other error -> fallback
    }

    // Fallback: just enumerate running processes, set 0%
    unsigned int count = 256;
    std::vector<nvmlProcessInfo_t> procs(count);
    auto st1 = nvmlDeviceGetComputeRunningProcesses_v2(handle_, &count, procs.data());
    if (st1 == NVML_ERROR_INSUFFICIENT_SIZE) {
        procs.resize(count);
        st1 = nvmlDeviceGetComputeRunningProcesses_v2(handle_, &count, procs.data());
    }
    if (st1 == NVML_SUCCESS) {
        for (unsigned i=0;i<count;i++) out[procs[i].pid] = 0;
    }
    count = 256;
    procs.assign(count, nvmlProcessInfo_t{});
    auto st2 = nvmlDeviceGetGraphicsRunningProcesses_v3(handle_, &count, procs.data());
    if (st2 == NVML_ERROR_INSUFFICIENT_SIZE) {
        procs.resize(count);
        st2 = nvmlDeviceGetGraphicsRunningProcesses_v3(handle_, &count, procs.data());
    }
    if (st2 == NVML_SUCCESS) {
        for (unsigned i=0;i<count;i++) out[procs[i].pid] = 0;
    }
    return out;
}

// -------------------- helpers --------------------

template <typename T>
static double avg_of(const std::vector<T>& v) {
    if (v.empty()) return 0.0;
    long double s = 0;
    for (auto& x : v) s += static_cast<long double>(x);
    return static_cast<double>(s / v.size());
}

template <typename T>
static double p95_of(std::vector<T> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = 0.95 * (v.size() - 1);
    const size_t i = static_cast<size_t>(pos);
    const double frac = pos - i;
    const double a = static_cast<double>(v[i]);
    const double b = static_cast<double>(v[std::min(i+1, v.size()-1)]);
    return a + (b - a) * frac;
}

// -------------------- MonitorSession --------------------

void MonitorSession::start(unsigned gpuIdx) {
    if (running.exchange(true)) return;
    gpu_index = gpuIdx;
    t0 = Clock::now();
    t_last = t0;

    worker = std::thread([this]() {
        try {
            NvmlDevice dev(gpu_index);

            // initial reads
            unsigned int pow_mw=0, ug=0, um=0, clk=0;
            unsigned long long mem_used=0;
            dev.getPowerMilliwatts(pow_mw);
            dev.getUtilization(ug, um);
            dev.getMemoryUsed(mem_used);
            dev.getSmClock(clk);

            auto perpid = dev.getPerPidGpuPercent(0ULL);
            for (auto& kv : perpid) if (kv.second > 0) pid_energy_possible = true;

            last_power_mw = pow_mw;

            samples.reserve(4096);
            series_util_gpu.reserve(4096);
            series_util_mem.reserve(4096);
            series_mem_used.reserve(4096);
            series_sm_clock.reserve(4096);

            while (running.load(std::memory_order_relaxed)) {
                auto now = Clock::now();
                double dt = std::chrono::duration<double>(now - t_last).count();
                double tsec = std::chrono::duration<double>(now - t0).count();

                // current
                dev.getPowerMilliwatts(pow_mw);
                dev.getUtilization(ug, um);
                dev.getMemoryUsed(mem_used);
                dev.getSmClock(clk);
                perpid = dev.getPerPidGpuPercent(0ULL);
                for (auto& kv : perpid) if (kv.second > 0) pid_energy_possible = true;

                // energy trapezoid
                double p1_w = last_power_mw / 1000.0;
                double p2_w = pow_mw / 1000.0;
                double p_avg = 0.5 * (p1_w + p2_w);
                energy_joules += p_avg * dt;
                last_power_mw = pow_mw;
                t_last = now;

                // per-PID attribution
                if (pid_energy_possible && !perpid.empty()) {
                    double total_pct = 0.0;
                    for (auto& kv : perpid) total_pct += kv.second;
                    if (total_pct > 0) {
                        for (auto& kv : perpid) {
                            double frac = (kv.second / 100.0);
                            // Optionally renormalize to sum=1:
                            // frac = (kv.second / total_pct);
                            pid_energy_j[kv.first] += p_avg * dt * frac;
                        }
                    }
                }

                // peaks
                peak_util_gpu = std::max(peak_util_gpu, ug);
                peak_util_mem = std::max(peak_util_mem, um);
                peak_mem_used_bytes = std::max(peak_mem_used_bytes, mem_used);
                peak_clocks_sm_mhz = std::max(peak_clocks_sm_mhz, clk);

                // series
                series_util_gpu.push_back(ug);
                series_util_mem.push_back(um);
                series_mem_used.push_back(mem_used);
                series_sm_clock.push_back(clk);

                // sample
                NvmlSample s;
                s.ts_sec = tsec;
                s.power_mw = pow_mw;
                s.util_gpu = ug;
                s.util_mem = um;
                s.mem_used_bytes = mem_used;
                s.clocks_sm_mhz = clk;
                s.pid_gpu_percent = perpid;
                samples.push_back(std::move(s));

                std::this_thread::sleep_for(interval);
            }
        } catch (const std::exception& e) {
            std::cerr << "[monitor] Exception: " << e.what() << std::endl;
            running = false;
        }
    });
}

NvmlSummary MonitorSession::stopAndSummarize() {
    if (running.exchange(false)) {
        if (worker.joinable()) worker.join();
    }
    NvmlSummary sum{};
    if (samples.empty()) return sum;

    double duration = std::chrono::duration<double>(t_last - t0).count();
    sum.duration_sec = duration;
    sum.energy_joules = energy_joules;
    sum.energy_wh = energy_joules / 3600.0;
    sum.avg_power_w = duration > 0 ? (energy_joules / duration) : 0.0;

    sum.peak_util_gpu = peak_util_gpu;
    sum.peak_util_mem = peak_util_mem;
    sum.peak_mem_used_bytes = peak_mem_used_bytes;
    sum.peak_clocks_sm_mhz = peak_clocks_sm_mhz;

    sum.avg_gpu_util = avg_of(series_util_gpu);
    sum.p95_gpu_util = p95_of(series_util_gpu);
    sum.avg_mem_util = avg_of(series_util_mem);
    sum.p95_mem_util = p95_of(series_util_mem);
    sum.avg_mem_used_bytes = avg_of(series_mem_used);
    sum.avg_sm_clock_mhz = avg_of(series_sm_clock);

    // Per-PID averages & energy
    std::map<unsigned int, std::pair<double,int>> acc;
    for (const auto& s : samples) {
        for (const auto& kv : s.pid_gpu_percent) {
            acc[kv.first].first  += kv.second;
            acc[kv.first].second += 1;
        }
    }
    for (auto& kv : acc) {
        sum.pid_avg_gpu_percent[kv.first] =
            kv.second.second ? (kv.second.first / kv.second.second) : 0.0;
    }
    sum.pid_energy_estimated = pid_energy_possible;
    if (pid_energy_possible) sum.pid_energy_joules = pid_energy_j;

    sum.samples_count = samples.size();
    return sum;
}

json MonitorSession::summaryToJson(const NvmlSummary& s) const {
    json o;
    o["chunk_id"] = chunk_id;
    o["duration_sec"] = s.duration_sec;
    o["energy_joules"] = s.energy_joules;
    o["energy_wh"] = s.energy_wh;
    o["avg_power_w"] = s.avg_power_w;

    o["avg_gpu_util"] = s.avg_gpu_util;
    o["p95_gpu_util"] = s.p95_gpu_util;
    o["avg_mem_util"] = s.avg_mem_util;
    o["p95_mem_util"] = s.p95_mem_util;
    o["avg_mem_used_bytes"] = s.avg_mem_used_bytes;
    o["avg_sm_clock_mhz"] = s.avg_sm_clock_mhz;

    o["peak_util_gpu"] = s.peak_util_gpu;
    o["peak_util_mem"] = s.peak_util_mem;
    o["peak_mem_used_bytes"] = s.peak_mem_used_bytes;
    o["peak_clocks_sm_mhz"] = s.peak_clocks_sm_mhz;

    // PID maps serialize naturally with nlohmann::json if we convert keys to strings
    json pid_avg = json::object();
    for (auto& kv : s.pid_avg_gpu_percent) pid_avg[std::to_string(kv.first)] = kv.second;
    o["pid_avg_gpu_percent"] = pid_avg;

    json pid_e = json::object();
    for (auto& kv : s.pid_energy_joules) pid_e[std::to_string(kv.first)] = kv.second;
    o["pid_energy_joules_estimated"] = pid_e;

    o["samples_count"] = s.samples_count;

    // light preview: first/last 3
    json head = json::array();
    for (size_t i=0;i<std::min<size_t>(3, samples.size()); ++i) {
        const auto& s = samples[i];
        head.push_back({
            {"t", s.ts_sec},
            {"p_mw", s.power_mw},
            {"ug", s.util_gpu},
            {"um", s.util_mem},
            {"mu_b", s.mem_used_bytes},
            {"sm_mhz", s.clocks_sm_mhz}
        });
    }
    json tail = json::array();
    for (size_t i=(samples.size()>=3? samples.size()-3:0); i<samples.size(); ++i) {
        const auto& s = samples[i];
        tail.push_back({
            {"t", s.ts_sec},
            {"p_mw", s.power_mw},
            {"ug", s.util_gpu},
            {"um", s.util_mem},
            {"mu_b", s.mem_used_bytes},
            {"sm_mhz", s.clocks_sm_mhz}
        });
    }
    o["samples_head"] = head;
    o["samples_tail"] = tail;

    return o;
}

// ------ Logging ------
void MonitorSession::ensure_logs_dir() {
    namespace fs = std::filesystem;
    fs::path p = fs::path("logs");
    if (!fs::exists(p)) fs::create_directories(p);
}

void MonitorSession::append_csv_header_if_needed(const std::filesystem::path& fpath) {
    if (std::filesystem::exists(fpath) && std::filesystem::file_size(fpath) > 0) return;
    std::ofstream out(fpath, std::ios::app);
    out << "timestamp_iso,chunk_id,gpu_index,duration_sec,energy_j,energy_Wh,avg_power_W,"
           "avg_gpu_util,p95_gpu_util,avg_mem_util,p95_mem_util,avg_mem_used_bytes,avg_sm_clock_mhz,"
           "peak_util_gpu,peak_util_mem,peak_mem_used_bytes,peak_clocks_sm_mhz,samples_count,"
           "pid_avg_gpu_percent_json,pid_energy_joules_json\n";
}

std::string MonitorSession::now_iso8601() {
    using sysclock = std::chrono::system_clock;
    auto now = sysclock::now();
    std::time_t tt = sysclock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

void MonitorSession::write_chunk_logs(const NvmlSummary& s, const json& as_json, bool write_samples_csv) const {
    ensure_logs_dir();

    // CSV
    std::filesystem::path csv = std::filesystem::path("logs") / "gpu_chunks.csv";
    append_csv_header_if_needed(csv);
    std::ofstream out(csv, std::ios::app);

    json pid_avg = json::object();
    for (auto& kv : s.pid_avg_gpu_percent) pid_avg[std::to_string(kv.first)] = kv.second;
    json pid_e = json::object();
    for (auto& kv : s.pid_energy_joules) pid_e[std::to_string(kv.first)] = kv.second;

    out << now_iso8601() << ","
        << "\"" << chunk_id << "\"" << ","
        << gpu_index << ","
        << s.duration_sec << ","
        << s.energy_joules << ","
        << s.energy_wh << ","
        << s.avg_power_w << ","
        << s.avg_gpu_util << ","
        << s.p95_gpu_util << ","
        << s.avg_mem_util << ","
        << s.p95_mem_util << ","
        << static_cast<long double>(s.avg_mem_used_bytes) << ","
        << s.avg_sm_clock_mhz << ","
        << s.peak_util_gpu << ","
        << s.peak_util_mem << ","
        << static_cast<unsigned long long>(s.peak_mem_used_bytes) << ","
        << s.peak_clocks_sm_mhz << ","
        << s.samples_count << ","
        << "\"" << pid_avg.dump() << "\","
        << "\"" << pid_e.dump() << "\"\n";

    // NDJSON
    std::filesystem::path nd = std::filesystem::path("logs") / "gpu_chunks.ndjson";
    std::ofstream ndout(nd, std::ios::app);
    json entry = as_json;
    entry["timestamp_iso"] = now_iso8601();
    entry["gpu_index"] = gpu_index;
    ndout << entry.dump() << "\n";

    // Optional: per-sample CSV
    if (write_samples_csv) {
        std::filesystem::path sfile = std::filesystem::path("logs") / ("chunk_" + chunk_id + "_samples.csv");
        std::ofstream sout(sfile);
        sout << "t_sec,power_mw,util_gpu,util_mem,mem_used_bytes,sm_clock_mhz\n";
        for (const auto& smp : samples) {
            sout << smp.ts_sec << ","
                 << smp.power_mw << ","
                 << smp.util_gpu << ","
                 << smp.util_mem << ","
                 << smp.mem_used_bytes << ","
                 << smp.clocks_sm_mhz << "\n";
        }
    }
}

// -------------------- MonitorManager --------------------

MonitorManager& MonitorManager::instance() {
    static MonitorManager m;
    return m;
}

std::shared_ptr<MonitorSession>
MonitorManager::start(const std::string& chunk_id, unsigned gpu_index, std::optional<unsigned> pid_hint) {
    std::lock_guard<std::mutex> lk(mu_);
    if (sessions_.count(chunk_id)) return nullptr;
    auto s = std::make_shared<MonitorSession>();
    s->chunk_id = chunk_id;
    if (pid_hint) s->hinted_pid = *pid_hint;
    s->start(gpu_index);
    sessions_[chunk_id] = s;
    return s;
}

json MonitorManager::end(const std::string& chunk_id) {
    std::shared_ptr<MonitorSession> s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(chunk_id);
        if (it == sessions_.end()) return json{{"error","unknown_chunk_id"}};
        s = it->second;
        sessions_.erase(it);
    }
    auto sum = s->stopAndSummarize();
    auto j = s->summaryToJson(sum);
    s->write_chunk_logs(sum, j, /*write_samples_csv=*/false);
    return j;
}

void MonitorManager::stopAll() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : sessions_) kv.second->stopAndSummarize();
    sessions_.clear();
}

// -------------------- WsSession / WsListener --------------------

WsSession::WsSession(tcp::socket socket)
    : ws_(std::move(socket)) {}

void WsSession::run() {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(beast::http::field::server, std::string("gpu-listener/2.0"));
        }
    ));
    ws_.async_accept(beast::bind_front_handler(&WsSession::onAccept, shared_from_this()));
}

void WsSession::onAccept(beast::error_code ec) {
    if (ec) return;
    doRead();
}

void WsSession::doRead() {
    ws_.async_read(buffer_, beast::bind_front_handler(&WsSession::onRead, shared_from_this()));
}

void WsSession::onRead(beast::error_code ec, std::size_t) {
    if (ec == websocket::error::closed) return;
    if (ec) { fail(ec, "read"); return; }

    std::string text = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    json msg;
    try { msg = json::parse(text); }
    catch (...) { write(errorMsg("invalid_json","parse failed")); doRead(); return; }

    handleMessage(msg);
    doRead();
}

void WsSession::write(const json& j) {
    auto s = j.dump();
    send_queue_.push_back(std::move(s));
    if (send_queue_.size() == 1) doWrite();
}

void WsSession::doWrite() {
    ws_.async_write(boost::asio::buffer(send_queue_.front()),
                    beast::bind_front_handler(&WsSession::onWrite, shared_from_this()));
}

void WsSession::onWrite(beast::error_code ec, std::size_t) {
    if (ec) { fail(ec, "write"); return; }
    send_queue_.pop_front();
    if (!send_queue_.empty()) doWrite();
}

json WsSession::errorMsg(const std::string& code, const std::string& msg) {
    return json{{"type","error"},{"code",code},{"message",msg}};
}

void WsSession::handleMessage(const json& obj) {
    if (!obj.contains("type") || obj["type"] != "chunk_status") {
        write(errorMsg("unknown_type","Expected type='chunk_status'"));
        return;
    }
    if (!obj.contains("chunk_id") || !obj.contains("status")) {
        write(errorMsg("bad_message","Missing chunk_id or status"));
        return;
    }
    const std::string cid = obj["chunk_id"].get<std::string>();
    const int status = obj["status"].get<int>();
    unsigned gpu_index = 0;
    if (obj.contains("gpu_index")) {
        auto gi = obj["gpu_index"];
        if (gi.is_number_integer()) gpu_index = static_cast<unsigned>(gi.get<int>() < 0 ? 0 : gi.get<int>());
        else if (gi.is_number_unsigned()) gpu_index = gi.get<unsigned>();
    }
    std::optional<unsigned> pid;
    if (obj.contains("pid") && obj["pid"].is_number_integer()) {
        int p = obj["pid"].get<int>();
        if (p >= 0) pid = static_cast<unsigned>(p);
    }

    if (status == 0) {
        auto s = MonitorManager::instance().start(cid, gpu_index, pid);
        if (!s) { write(errorMsg("already_running","Monitoring already running for this chunk_id")); return; }
        write(json{{"type","ack"},{"chunk_id",cid},{"status",0}});
    } else if (status == 1 || status == -1) {
        auto sum = MonitorManager::instance().end(cid);
        sum["type"] = (status==1 ? "summary" : "summary_error");
        write(sum);
    } else {
        write(errorMsg("bad_status","status must be 0|1|-1"));
    }
}

void WsSession::fail(beast::error_code ec, const char* what) {
    if (ec == boost::asio::error::operation_aborted) return;
    std::cerr << "[ws] " << what << ": " << ec.message() << "\n";
}

WsListener::WsListener(boost::asio::io_context& ioc, tcp::endpoint ep)
    : ioc_(ioc), acceptor_(ioc) {
    beast::error_code ec;
    acceptor_.open(ep.protocol(), ec); if (ec) return fail(ec, "open");
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec); if (ec) return fail(ec, "set_option");
    acceptor_.bind(ep, ec); if (ec) return fail(ec, "bind");
    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec); if (ec) return fail(ec, "listen");
}
void WsListener::run() { doAccept(); }
void WsListener::doAccept() {
    acceptor_.async_accept(
        boost::asio::make_strand(ioc_),
        beast::bind_front_handler(&WsListener::onAccept, shared_from_this()));
}
void WsListener::onAccept(beast::error_code ec, tcp::socket socket) {
    if (!ec) std::make_shared<WsSession>(std::move(socket))->run();
    else fail(ec, "accept");
    doAccept();
}
void WsListener::fail(beast::error_code ec, const char* what) {
    if (ec == boost::asio::error::operation_aborted) return;
    std::cerr << "[listener] " << what << ": " << ec.message() << "\n";
}

// -------------------- Server runner --------------------

int run_server(unsigned short port, const std::string& host) {
    // NVML init early to report errors upfront
    nvmlReturn_t st = nvmlInit_v2();
    if (st != NVML_SUCCESS && st != NVML_ERROR_ALREADY_INITIALIZED) {
        std::cerr << "NVML init failed at startup: " << nvmlErrorStr(st) << "\n";
        return 1;
    }

    boost::asio::io_context ioc{1};
    auto ep = tcp::endpoint(boost::asio::ip::make_address(host), port);
    auto srv = std::make_shared<WsListener>(ioc, ep);
    srv->run();

    std::atomic<bool> stop{false};
    auto onSignal = +[](int){};
#if defined(_WIN32)
    // Windows: Ctrl+C is handled differently; relying on external stop is fine.
#else
    std::signal(SIGINT, [](int){ MonitorManager::instance().stopAll(); });
    std::signal(SIGTERM, [](int){ MonitorManager::instance().stopAll(); });
#endif

    std::thread t([&](){ ioc.run(); });

    std::cout << "GPU listener on ws://" << host << ":" << port << "  (Ctrl+C to stop)\n";
#if defined(_WIN32)
    // Busy-wait minimal loop; in real apps, hook proper console ctl handler
    while (true) std::this_thread::sleep_for(std::chrono::milliseconds(200));
#else
    // Sleep until process is terminated; stopAll called in signal handler
    while (true) std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif

    // (Not usually reached)
    MonitorManager::instance().stopAll();
    ioc.stop();
    t.join();
    nvmlShutdown();
    return 0;
}

} // namespace gpu_listener

// -------------------- main --------------------
int main(int argc, char** argv) {
    unsigned short port = 8765;
    std::string host = "127.0.0.1";
    if (argc >= 2) port = static_cast<unsigned short>(std::stoi(argv[1]));
    if (argc >= 3) host = argv[2];
    return gpu_listener::run_server(port, host);
}
