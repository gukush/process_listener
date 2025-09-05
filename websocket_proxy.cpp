// websocket_proxy.cpp
#include "websocket_proxy.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

namespace unified_monitor {

using json = nlohmann::json;

// ============= ProxySession Implementation =============

ProxySession::ProxySession(tcp::socket&& socket,
                         net::io_context& ioc,
                         const std::string& upstream_host,
                         uint16_t upstream_port,
                         EnhancedMessageInterceptor* interceptor)
    : client_ws_(std::move(socket))
    , server_ws_(net::make_strand(ioc))
    , upstream_host_(upstream_host)
    , upstream_port_(upstream_port)
    , interceptor_(interceptor)
    , strand_(client_ws_.get_executor()) {
}

void ProxySession::run() {
    // Set WebSocket options for client connection
    client_ws_.set_option(websocket::stream_base::timeout::suggested(
        beast::role_type::server));
    
    client_ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(beast::http::field::server, "BeastProxy/1.0");
        }));
    
    // Accept WebSocket connection from client
    client_ws_.async_accept(
        beast::bind_front_handler(&ProxySession::on_client_accept, 
                                 shared_from_this()));
}

void ProxySession::on_client_accept(beast::error_code ec) {
    if (ec) {
        std::cerr << "[PROXY] Client accept error: " << ec.message() << std::endl;
        return;
    }
    
    client_connected_ = true;
    std::cout << "[PROXY] Client connected" << std::endl;
    
    // Now connect to upstream server
    auto resolver = std::make_shared<tcp::resolver>(strand_);
    resolver->async_resolve(
        upstream_host_,
        std::to_string(upstream_port_),
        [self = shared_from_this(), resolver](beast::error_code ec, 
                                              tcp::resolver::results_type results) {
            if (ec) {
                std::cerr << "[PROXY] Resolver error: " << ec.message() << std::endl;
                return;
            }
            
            // Connect to upstream
            beast::get_lowest_layer(self->server_ws_).async_connect(
                results,
                beast::bind_front_handler(&ProxySession::on_server_connect, self));
        });
    
    // Start reading from client
    do_client_read();
}

void ProxySession::on_server_connect(beast::error_code ec, 
                                    tcp::resolver::results_type::endpoint_type ep) {
    if (ec) {
        std::cerr << "[PROXY] Server connect error: " << ec.message() << std::endl;
        close_connections();
        return;
    }
    
    std::cout << "[PROXY] Connected to upstream server: " 
              << ep.address() << ":" << ep.port() << std::endl;
    
    // Set timeout options
    server_ws_.set_option(websocket::stream_base::timeout::suggested(
        beast::role_type::client));
    
    // Perform WebSocket handshake
    server_ws_.async_handshake(
        upstream_host_, "/",
        beast::bind_front_handler(&ProxySession::on_server_handshake, 
                                 shared_from_this()));
}

void ProxySession::on_server_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[PROXY] Server handshake error: " << ec.message() << std::endl;
        close_connections();
        return;
    }
    
    server_connected_ = true;
    std::cout << "[PROXY] WebSocket handshake with server completed" << std::endl;
    
    // Start reading from server
    do_server_read();
}

// ---- Client Read/Write ----

void ProxySession::do_client_read() {
    client_ws_.async_read(
        client_buffer_,
        beast::bind_front_handler(&ProxySession::on_client_read, 
                                 shared_from_this()));
}

void ProxySession::on_client_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec == websocket::error::closed) {
        std::cout << "[PROXY] Client disconnected" << std::endl;
        close_connections();
        return;
    }
    
    if (ec) {
        std::cerr << "[PROXY] Client read error: " << ec.message() << std::endl;
        close_connections();
        return;
    }
    
    // Extract message
    std::string message = beast::buffers_to_string(client_buffer_.data());
    client_buffer_.consume(bytes_transferred);
    
    // Intercept and analyze
    intercept_client_to_server(message);
    
    // Forward to server
    if (server_connected_) {
        std::lock_guard<std::mutex> lock(server_queue_mutex_);
        bool write_in_progress = !server_send_queue_.empty();
        server_send_queue_.push(message);
        
        if (!write_in_progress) {
            do_server_write();
        }
    }
    
    // Continue reading
    do_client_read();
}

void ProxySession::do_server_read() {
    server_ws_.async_read(
        server_buffer_,
        beast::bind_front_handler(&ProxySession::on_server_read,
                                 shared_from_this()));
}

void ProxySession::on_server_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec == websocket::error::closed) {
        std::cout << "[PROXY] Server disconnected" << std::endl;
        close_connections();
        return;
    }
    
    if (ec) {
        std::cerr << "[PROXY] Server read error: " << ec.message() << std::endl;
        close_connections();
        return;
    }
    
    // Extract message
    std::string message = beast::buffers_to_string(server_buffer_.data());
    server_buffer_.consume(bytes_transferred);
    
    // Intercept and analyze
    intercept_server_to_client(message);
    
    // Forward to client
    if (client_connected_) {
        std::lock_guard<std::mutex> lock(client_queue_mutex_);
        bool write_in_progress = !client_send_queue_.empty();
        client_send_queue_.push(message);
        
        if (!write_in_progress) {
            do_client_write();
        }
    }
    
    // Continue reading
    do_server_read();
}

void ProxySession::do_client_write() {
    std::lock_guard<std::mutex> lock(client_queue_mutex_);
    if (client_send_queue_.empty()) return;
    
    client_ws_.async_write(
        net::buffer(client_send_queue_.front()),
        beast::bind_front_handler(&ProxySession::on_client_write,
                                 shared_from_this()));
}

void ProxySession::on_client_write(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        std::cerr << "[PROXY] Client write error: " << ec.message() << std::endl;
        close_connections();
        return;
    }
    
    std::lock_guard<std::mutex> lock(client_queue_mutex_);
    client_send_queue_.pop();
    
    if (!client_send_queue_.empty()) {
        do_client_write();
    }
}

void ProxySession::do_server_write() {
    std::lock_guard<std::mutex> lock(server_queue_mutex_);
    if (server_send_queue_.empty()) return;
    
    server_ws_.async_write(
        net::buffer(server_send_queue_.front()),
        beast::bind_front_handler(&ProxySession::on_server_write,
                                 shared_from_this()));
}

void ProxySession::on_server_write(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        std::cerr << "[PROXY] Server write error: " << ec.message() << std::endl;
        close_connections();
        return;
    }
    
    std::lock_guard<std::mutex> lock(server_queue_mutex_);
    server_send_queue_.pop();
    
    if (!server_send_queue_.empty()) {
        do_server_write();
    }
}

// ---- Message Interception ----

void ProxySession::intercept_client_to_server(const std::string& message) {
    try {
        // Try to parse as JSON
        json msg = json::parse(message);
        
        // Check for chunk results (client -> server)
        if (msg.contains("type") && msg["type"] == "chunk:result") {
            interceptor_->onChunkResult(msg);
            std::cout << "[PROXY] ← Chunk result: " << msg["chunkId"] 
                     << " status: " << msg["status"] << std::endl;
        }
        
        // Socket.io format: "42["event", data]"
        if (message.size() > 2 && message[0] == '4' && message[1] == '2') {
            size_t bracket = message.find('[');
            if (bracket != std::string::npos) {
                json arr = json::parse(message.substr(bracket));
                if (arr.is_array() && arr.size() >= 2) {
                    std::string event = arr[0];
                    if (event == "chunk:result") {
                        interceptor_->onChunkResult(arr[1]);
                        std::cout << "[PROXY] ← Chunk result (socket.io): " 
                                 << arr[1]["chunkId"] << std::endl;
                    }
                }
            }
        }
        
    } catch (...) {
        // Not JSON or not a message we care about
    }
}

void ProxySession::intercept_server_to_client(const std::string& message) {
    try {
        // Direct WebSocket JSON
        json msg = json::parse(message);
        
        if (msg.contains("type")) {
            if (msg["type"] == "task:init") {
                interceptor_->onTaskInit(msg);
                std::cout << "[PROXY] → Task init: " << msg["taskId"] << std::endl;
            } else if (msg["type"] == "chunk:assign") {
                interceptor_->onChunkAssign(msg);
                std::cout << "[PROXY] → Chunk assign: " << msg["chunkId"]
                         << " for task: " << msg["taskId"] << std::endl;
            }
        }
        
        // Socket.io format
        if (message.size() > 2 && message[0] == '4' && message[1] == '2') {
            size_t bracket = message.find('[');
            if (bracket != std::string::npos) {
                json arr = json::parse(message.substr(bracket));
                if (arr.is_array() && arr.size() >= 2) {
                    std::string event = arr[0];
                    json data = arr[1];
                    
                    if (event == "task:init") {
                        interceptor_->onTaskInit(data);
                        std::cout << "[PROXY] → Task init (socket.io): " 
                                 << data["taskId"] << std::endl;
                    } else if (event == "chunk:assign") {
                        interceptor_->onChunkAssign(data);
                        std::cout << "[PROXY] → Chunk assign (socket.io): " 
                                 << data["chunkId"] << std::endl;
                    }
                }
            }
        }
        
    } catch (...) {
        // Not JSON or not a message we care about
    }
}

void ProxySession::close_connections() {
    beast::error_code ec;
    
    if (client_connected_) {
        client_ws_.close(websocket::close_code::normal, ec);
        client_connected_ = false;
    }
    
    if (server_connected_) {
        server_ws_.close(websocket::close_code::normal, ec);
        server_connected_ = false;
    }
}

// ============= BeastProxyListener Implementation =============

BeastProxyListener::BeastProxyListener(net::io_context& ioc,
                                     tcp::endpoint endpoint,
                                     const std::string& upstream_host,
                                     uint16_t upstream_port,
                                     EnhancedMessageInterceptor* interceptor)
    : ioc_(ioc)
    , acceptor_(net::make_strand(ioc))
    , upstream_host_(upstream_host)
    , upstream_port_(upstream_port)
    , interceptor_(interceptor) {
    
    beast::error_code ec;
    
    // Open the acceptor
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        std::cerr << "[PROXY] Open error: " << ec.message() << std::endl;
        return;
    }
    
    // Allow address reuse
    acceptor_.set_option(net::socket_base::reuse_address(true), ec);
    if (ec) {
        std::cerr << "[PROXY] Set option error: " << ec.message() << std::endl;
        return;
    }
    
    // Bind to the server address
    acceptor_.bind(endpoint, ec);
    if (ec) {
        std::cerr << "[PROXY] Bind error: " << ec.message() << std::endl;
        return;
    }
    
    // Start listening
    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "[PROXY] Listen error: " << ec.message() << std::endl;
        return;
    }
}

void BeastProxyListener::run() {
    do_accept();
}

void BeastProxyListener::do_accept() {
    acceptor_.async_accept(
        net::make_strand(ioc_),
        beast::bind_front_handler(&BeastProxyListener::on_accept,
                                 shared_from_this()));
}

void BeastProxyListener::on_accept(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        std::cerr << "[PROXY] Accept error: " << ec.message() << std::endl;
    } else {
        // Create and run a new session
        std::make_shared<ProxySession>(
            std::move(socket),
            ioc_,
            upstream_host_,
            upstream_port_,
            interceptor_
        )->run();
    }
    
    // Accept another connection
    do_accept();
}

void BeastProxyListener::stop() {
    beast::error_code ec;
    acceptor_.close(ec);
}

// ============= BeastWebSocketProxy Implementation =============

BeastWebSocketProxy::BeastWebSocketProxy(ChunkTracker* tracker, 
                                       EnhancedMessageInterceptor* interceptor)
    : chunk_tracker_(tracker)
    , interceptor_(interceptor) {
}

BeastWebSocketProxy::~BeastWebSocketProxy() {
    stop();
}

bool BeastWebSocketProxy::start(uint16_t listen_port,
                               const std::string& upstream_host,
                               uint16_t upstream_port) {
    if (running_) return false;
    
    try {
        auto const address = net::ip::make_address("0.0.0.0");
        auto const endpoint = tcp::endpoint{address, listen_port};
        
        listener_ = std::make_unique<BeastProxyListener>(
            ioc_,
            endpoint,
            upstream_host,
            upstream_port,
            interceptor_
        );
        
        listener_->run();
        
        running_ = true;
        io_thread_ = std::thread([this]() {
            std::cout << "[PROXY] Starting on port " 
                     << endpoint.port() 
                     << " -> " << upstream_host << ":" << upstream_port << std::endl;
            ioc_.run();
        });
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "[PROXY] Start failed: " << e.what() << std::endl;
        return false;
    }
}

void BeastWebSocketProxy::stop() {
    if (!running_) return;
    
    running_ = false;
    
    if (listener_) {
        listener_->stop();
    }
    
    ioc_.stop();
    
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
}

} // namespace unified_monitor
