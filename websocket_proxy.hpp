#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace unified_monitor {

class ChunkTracker; // from orchestrator.hpp

// Public interceptor interface the proxy will call into.
class EnhancedMessageInterceptor {
public:
    virtual ~EnhancedMessageInterceptor() = default;
    virtual void onTaskInit(const nlohmann::json& msg) = 0;
    virtual void onChunkAssign(const nlohmann::json& msg) = 0;
    virtual void onChunkResult(const nlohmann::json& msg) = 0;
};

// Minimal Beast-based WebSocket proxy.
// Listens on :listen_port and forwards to upstream_host:upstream_port.
class BeastWebSocketProxy {
public:
    BeastWebSocketProxy(ChunkTracker* tracker, EnhancedMessageInterceptor* interceptor);
    ~BeastWebSocketProxy();

    bool start(uint16_t listen_port, const std::string& upstream_host, uint16_t upstream_port);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::thread io_thread_;
};

} // namespace unified_monitor
