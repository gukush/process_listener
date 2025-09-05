// websocket_proxy.hpp
#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <string>
#include <queue>
#include <map>
#include <mutex>

namespace unified_monitor {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

class ChunkTracker;
class EnhancedMessageInterceptor;

// Forward declaration
class ProxySession;

// ============= Proxy Session (handles one client connection) =============
class ProxySession : public std::enable_shared_from_this<ProxySession> {
public:
    ProxySession(tcp::socket&& socket,
                net::io_context& ioc,
                const std::string& upstream_host,
                uint16_t upstream_port,
                EnhancedMessageInterceptor* interceptor);
    
    void run();
    
private:
    // Client-side connection (browser/client -> proxy)
    websocket::stream<beast::tcp_stream> client_ws_;
    beast::flat_buffer client_buffer_;
    
    // Server-side connection (proxy -> upstream server)
    websocket::stream<beast::tcp_stream> server_ws_;
    beast::flat_buffer server_buffer_;
    
    // Upstream server details
    std::string upstream_host_;
    uint16_t upstream_port_;
    
    // Message queues for writing
    std::queue<std::string> client_send_queue_;
    std::queue<std::string> server_send_queue_;
    std::mutex client_queue_mutex_;
    std::mutex server_queue_mutex_;
    
    // Message interceptor
    EnhancedMessageInterceptor* interceptor_;
    
    // Strand to ensure thread safety
    net::strand<net::io_context::executor_type> strand_;
    
    // Connection state
    bool client_connected_{false};
    bool server_connected_{false};
    
    // Async operation handlers
    void on_client_accept(beast::error_code ec);
    void on_server_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void on_server_handshake(beast::error_code ec);
    
    // Read operations
    void do_client_read();
    void on_client_read(beast::error_code ec, std::size_t bytes_transferred);
    void do_server_read();
    void on_server_read(beast::error_code ec, std::size_t bytes_transferred);
    
    // Write operations
    void do_client_write();
    void on_client_write(beast::error_code ec, std::size_t bytes_transferred);
    void do_server_write();
    void on_server_write(beast::error_code ec, std::size_t bytes_transferred);
    
    // Message interception
    void intercept_client_to_server(const std::string& message);
    void intercept_server_to_client(const std::string& message);
    
    // Utility
    void close_connections();
};

// ============= Proxy Listener (accepts client connections) =============
class BeastProxyListener : public std::enable_shared_from_this<BeastProxyListener> {
public:
    BeastProxyListener(net::io_context& ioc,
                      tcp::endpoint endpoint,
                      const std::string& upstream_host,
                      uint16_t upstream_port,
                      EnhancedMessageInterceptor* interceptor);
    
    void run();
    void stop();
    
private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    std::string upstream_host_;
    uint16_t upstream_port_;
    EnhancedMessageInterceptor* interceptor_;
    
    void do_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);
};

// ============= Main Proxy Server =============
class BeastWebSocketProxy {
public:
    BeastWebSocketProxy(ChunkTracker* tracker, EnhancedMessageInterceptor* interceptor);
    ~BeastWebSocketProxy();
    
    bool start(uint16_t listen_port, 
              const std::string& upstream_host,
              uint16_t upstream_port);
    void stop();
    
private:
    net::io_context ioc_;
    std::unique_ptr<BeastProxyListener> listener_;
    std::thread io_thread_;
    
    ChunkTracker* chunk_tracker_;
    EnhancedMessageInterceptor* interceptor_;
    std::atomic<bool> running_{false};
};

} // namespace unified_monitor
