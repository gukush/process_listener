#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <thread>
#include <atomic>
#include <sys/wait.h>
#include <signal.h>

#include <nlohmann/json.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

namespace experimental_orchestrator {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

enum class ClientVariant {
    BROWSER_ONLY,
    BROWSER_PLUS_CPP,
    CPP_ONLY,
    SERVER_ONLY
};

struct ProcessInfo {
    pid_t pid;
    std::string command;
    std::string role;  // "browser", "cpp_client", "server", etc.
    Clock::time_point start_time;
    bool is_alive = true;
};

struct ExperimentConfig {
    ClientVariant variant;
    std::string chunk_id;
    std::string server_url = "ws://localhost:8765";
    unsigned gpu_index = 0;
    
    // Process configurations
    std::string browser_path = "/usr/bin/google-chrome";
    std::string browser_url = "http://localhost:3000";
    std::string cpp_client_path = "./cpp_client";
    std::string server_path = "node server.js";
    
    // Monitoring options
    bool enable_gpu = true;
    bool enable_system = true;
    int monitoring_duration_sec = 60;
    
    // Browser specific
    std::string browser_profile_dir;
    std::vector<std::string> browser_args;
};

class ProcessManager {
public:
    explicit ProcessManager();
    ~ProcessManager();
    
    pid_t launchBrowser(const std::string& browser_path, 
                       const std::string& url,
                       const std::vector<std::string>& extra_args = {});
    
    pid_t launchCppClient(const std::string& client_path,
                         const std::vector<std::string>& args = {});
    
    pid_t launchServer(const std::string& server_cmd);
    
    void addProcess(pid_t pid, const std::string& command, const std::string& role);
    
    std::vector<pid_t> getAllPids() const;
    std::vector<pid_t> getPidsByRole(const std::string& role) const;
    
    bool isProcessAlive(pid_t pid) const;
    void updateProcessStates();
    void terminateAll();
    void waitForAllProcesses();
    
    const std::map<pid_t, ProcessInfo>& getProcesses() const { return processes_; }

private:
    std::map<pid_t, ProcessInfo> processes_;
    mutable std::mutex processes_mutex_;
    
    pid_t launchProcess(const std::vector<std::string>& cmd_args, const std::string& role);
};

class MonitoringClient {
public:
    explicit MonitoringClient(const std::string& server_url);
    ~MonitoringClient();
    
    bool connect();
    void disconnect();
    
    bool startMonitoring(const std::string& chunk_id,
                        ClientVariant variant,
                        const std::vector<pid_t>& pids,
                        unsigned gpu_index = 0,
                        bool enable_gpu = true,
                        bool enable_system = true);
    
    json stopMonitoring(const std::string& chunk_id);
    
    bool isConnected() const { return connected_; }

private:
    std::string server_url_;
    websocketpp::client<websocketpp::config::asio_client> ws_client_;
    websocketpp::connection_hdl connection_;
    std::atomic<bool> connected_{false};
    std::thread io_thread_;
    
    void onMessage(websocketpp::connection_hdl hdl, 
                  websocketpp::client<websocketpp::config::asio_client>::message_ptr msg);
    void onOpen(websocketpp::connection_hdl hdl);
    void onClose(websocketpp::connection_hdl hdl);
    
    void sendMessage(const json& message);
};

class ExperimentOrchestrator {
public:
    explicit ExperimentOrchestrator();
    ~ExperimentOrchestrator();
    
    bool runExperiment(const ExperimentConfig& config);
    
    // Individual experiment variants
    bool runBrowserOnly(const ExperimentConfig& config);
    bool runBrowserPlusCpp(const ExperimentConfig& config);
    bool runCppOnly(const ExperimentConfig& config);
    bool runServerOnly(const ExperimentConfig& config);
    
    void stop();
    
private:
    std::unique_ptr<ProcessManager> process_manager_;
    std::unique_ptr<MonitoringClient> monitor_client_;
    std::atomic<bool> running_{false};
    
    bool setupMonitoring(const ExperimentConfig& config, const std::vector<pid_t>& pids);
    json finalizeMonitoring(const std::string& chunk_id);
    void waitForExperiment(int duration_sec);
    
    std::string generateChunkId() const;
    ExperimentConfig createDefaultConfig(ClientVariant variant) const;
};

// Implementation

ProcessManager::ProcessManager() = default;

ProcessManager::~ProcessManager() {
    terminateAll();
}

pid_t ProcessManager::launchBrowser(const std::string& browser_path, 
                                   const std::string& url,
                                   const std::vector<std::string>& extra_args) {
    
    // Create unique profile directory
    std::string profile_dir = "/tmp/browser_profile_" + std::to_string(time(nullptr));
    
    std::vector<std::string> cmd_args = {
        browser_path,
        "--user-data-dir=" + profile_dir,
        "--no-sandbox",
        "--disable-web-security",
        "--disable-features=VizDisplayCompositor", // Better for GPU monitoring
        "--enable-gpu-benchmarking",
        "--enable-logging",
        "--log-level=0",
        url
    };
    
    // Add extra args
    cmd_args.insert(cmd_args.end(), extra_args.begin(), extra_args.end());
    
    return launchProcess(cmd_args, "browser");
}

pid_t ProcessManager::launchCppClient(const std::string& client_path,
                                     const std::vector<std::string>& args) {
    std::vector<std::string> cmd_args = {client_path};
    cmd_args.insert(cmd_args.end(), args.begin(), args.end());
    
    return launchProcess(cmd_args, "cpp_client");
}

pid_t ProcessManager::launchServer(const std::string& server_cmd) {
    // Parse command string into args (simple split by space)
    std::vector<std::string> cmd_args;
    std::istringstream iss(server_cmd);
    std::string arg;
    while (iss >> arg) {
        cmd_args.push_back(arg);
    }
    
    return launchProcess(cmd_args, "server");
}

pid_t ProcessManager::launchProcess(const std::vector<std::string>& cmd_args, const std::string& role) {
    if (cmd_args.empty()) return -1;
    
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        std::vector<char*> c_args;
        for (const auto& arg : cmd_args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);
        
        execv(cmd_args[0].c_str(), c_args.data());
        exit(1); // exec failed
    }
    
    if (pid > 0) {
        std::lock_guard<std::mutex> lock(processes_mutex_);
        ProcessInfo info;
        info.pid = pid;
        info.command = cmd_args[0];
        for (size_t i = 1; i < cmd_args.size(); ++i) {
            info.command += " " + cmd_args[i];
        }
        info.role = role;
        info.start_time = Clock::now();
        info.is_alive = true;
        
        processes_[pid] = info;
        
        std::cout << "[Orchestrator] Launched " << role << " process (PID: " << pid << "): " 
                  << info.command << std::endl;
    }
    
    return pid;
}

bool ProcessManager::isProcessAlive(pid_t pid) const {
    return kill(pid, 0) == 0;
}

void ProcessManager::updateProcessStates() {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    for (auto& [pid, info] : processes_) {
        info.is_alive = isProcessAlive(pid);
    }
}

std::vector<pid_t> ProcessManager::getAllPids() const {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    std::vector<pid_t> pids;
    for (const auto& [pid, info] : processes_) {
        if (info.is_alive) {
            pids.push_back(pid);
        }
    }
    return pids;
}

std::vector<pid_t> ProcessManager::getPidsByRole(const std::string& role) const {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    std::vector<pid_t> pids;
    for (const auto& [pid, info] : processes_) {
        if (info.is_alive && info.role == role) {
            pids.push_back(pid);
        }
    }
    return pids;
}

void ProcessManager::terminateAll() {
    std::lock_guard<std::mutex> lock(processes_mutex_);
    for (const auto& [pid, info] : processes_) {
        if (info.is_alive) {
            std::cout << "[Orchestrator] Terminating " << info.role << " (PID: " << pid << ")" << std::endl;
            kill(pid, SIGTERM);
        }
    }
    
    // Give processes time to terminate gracefully
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Force kill if needed
    for (const auto& [pid, info] : processes_) {
        if (isProcessAlive(pid)) {
            std::cout << "[Orchestrator] Force killing " << info.role << " (PID: " << pid << ")" << std::endl;
            kill(pid, SIGKILL);
        }
    }
}

// ExperimentOrchestrator implementation
bool ExperimentOrchestrator::runExperiment(const ExperimentConfig& config) {
    running_ = true;
    
    process_manager_ = std::make_unique<ProcessManager>();
    monitor_client_ = std::make_unique<MonitoringClient>(config.server_url);
    
    if (!monitor_client_->connect()) {
        std::cerr << "[Orchestrator] Failed to connect to monitoring server" << std::endl;
        return false;
    }
    
    bool success = false;
    switch (config.variant) {
        case ClientVariant::BROWSER_ONLY:
            success = runBrowserOnly(config);
            break;
        case ClientVariant::BROWSER_PLUS_CPP:
            success = runBrowserPlusCpp(config);
            break;
        case ClientVariant::CPP_ONLY:
            success = runCppOnly(config);
            break;
        case ClientVariant::SERVER_ONLY:
            success = runServerOnly(config);
            break;
    }
    
    // Finalize monitoring and cleanup
    auto results = finalizeMonitoring(config.chunk_id);
    std::cout << "[Orchestrator] Experiment results: " << results.dump(2) << std::endl;
    
    process_manager_->terminateAll();
    monitor_client_->disconnect();
    
    running_ = false;
    return success;
}

bool ExperimentOrchestrator::runBrowserOnly(const ExperimentConfig& config) {
    std::cout << "[Orchestrator] Starting BROWSER_ONLY experiment" << std::endl;
    
    // Launch browser
    pid_t browser_pid = process_manager_->launchBrowser(
        config.browser_path, 
        config.browser_url,
        config.browser_args
    );
    
    if (browser_pid <= 0) {
        std::cerr << "[Orchestrator] Failed to launch browser" << std::endl;
        return false;
    }
    
    // Wait a bit for browser to fully start
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    // Start monitoring
    std::vector<pid_t> pids = {browser_pid};
    if (!setupMonitoring(config, pids)) {
        return false;
    }
    
    // Run experiment
    waitForExperiment(config.monitoring_duration_sec);
    
    return true;
}

bool ExperimentOrchestrator::runBrowserPlusCpp(const ExperimentConfig& config) {
    std::cout << "[Orchestrator] Starting BROWSER_PLUS_CPP experiment" << std::endl;
    
    // Launch C++ client first
    pid_t cpp_pid = process_manager_->launchCppClient(config.cpp_client_path);
    if (cpp_pid <= 0) {
        std::cerr << "[Orchestrator] Failed to launch C++ client" << std::endl;
        return false;
    }
    
    // Launch browser
    pid_t browser_pid = process_manager_->launchBrowser(
        config.browser_path,
        config.browser_url,
        config.browser_args
    );
    
    if (browser_pid <= 0) {
        std::cerr << "[Orchestrator] Failed to launch browser" << std::endl;
        return false;
    }
    
    // Wait for both to start
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    // Start monitoring both processes
    std::vector<pid_t> pids = {browser_pid, cpp_pid};
    if (!setupMonitoring(config, pids)) {
        return false;
    }
    
    waitForExperiment(config.monitoring_duration_sec);
    
    return true;
}

bool ExperimentOrchestrator::runCppOnly(const ExperimentConfig& config) {
    std::cout << "[Orchestrator] Starting CPP_ONLY experiment" << std::endl;
    
    // Launch C++ client
    pid_t cpp_pid = process_manager_->launchCppClient(config.cpp_client_path);
    if (cpp_pid <= 0) {
        std::cerr << "[Orchestrator] Failed to launch C++ client" << std::endl;
        return false;
    }
    
    // Start monitoring (will auto-discover child processes)
    std::vector<pid_t> pids = {cpp_pid};
    if (!setupMonitoring(config, pids)) {
        return false;
    }
    
    waitForExperiment(config.monitoring_duration_sec);
    
    return true;
}

bool ExperimentOrchestrator::setupMonitoring(const ExperimentConfig& config, const std::vector<pid_t>& pids) {
    return monitor_client_->startMonitoring(
        config.chunk_id,
        config.variant,
        pids,
        config.gpu_index,
        config.enable_gpu,
        config.enable_system
    );
}

json ExperimentOrchestrator::finalizeMonitoring(const std::string& chunk_id) {
    return monitor_client_->stopMonitoring(chunk_id);
}

void ExperimentOrchestrator::waitForExperiment(int duration_sec) {
    std::cout << "[Orchestrator] Running experiment for " << duration_sec << " seconds..." << std::endl;
    
    for (int i = 0; i < duration_sec && running_; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // Check if processes are still alive
        process_manager_->updateProcessStates();
        
        // Optional: print progress every 10 seconds
        if ((i + 1) % 10 == 0) {
            std::cout << "[Orchestrator] " << (duration_sec - i - 1) << " seconds remaining..." << std::endl;
        }
    }
}

std::string ExperimentOrchestrator::generateChunkId() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "exp_" << time_t << "_" << (rand() % 1000);
    return ss.str();
}

} // namespace experimental_orchestrator

// Usage examples for WebGPU experiments:
int main(int argc, char** argv) {
    using namespace experimental_orchestrator;
    
    // WebGPU Browser-only experiment
    ExperimentConfig webgpu_config;
    webgpu_config.variant = ClientVariant::BROWSER_ONLY;
    webgpu_config.chunk_id = "webgpu_browser_001";
    webgpu_config.browser_url = "https://localhost:3000";
    webgpu_config.worker_id = "test1";
    webgpu_config.log_level = "debug";
    webgpu_config.mode = "headless";
    webgpu_config.use_hardware_vulkan = true;  // Use native Vulkan
    webgpu_config.monitoring_duration_sec = 60;
    
    // Browser + C++ hybrid experiment
    ExperimentConfig hybrid_config;
    hybrid_config.variant = ClientVariant::BROWSER_PLUS_CPP;
    hybrid_config.chunk_id = "hybrid_webgpu_001";
    hybrid_config.browser_url = "https://localhost:3000";
    hybrid_config.worker_id = "hybrid1";
    hybrid_config.cpp_client_path = "./build/gpu_worker";
    hybrid_config.use_hardware_vulkan = false;  // Use SwiftShader for consistency
    hybrid_config.monitoring_duration_sec = 120;
    
    // Run experiments
    ExperimentOrchestrator orchestrator;
    
    std::cout << "=== Running WebGPU Browser Experiment ===" << std::endl;
    bool success1 = orchestrator.runExperiment(webgpu_config);
    
    std::cout << "=== Running Hybrid WebGPU + C++ Experiment ===" << std::endl;  
    bool success2 = orchestrator.runExperiment(hybrid_config);
    
    return (success1 && success2) ? 0 : 1;
}
