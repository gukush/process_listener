#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <functional>
#include <memory>
#include <atomic>

class WebSocketListener {
public:
    using json = nlohmann::json;

    WebSocketListener();
    ~WebSocketListener();

    bool connect(const std::string& host, const std::string& port, const std::string& target="/ws-listener", bool use_ssl=true);
    void disconnect();
    bool isConnected() const;

    // Callback functions for metrics control
    std::function<void()> onStartMetrics;  // Called when server sends start message
    std::function<void()> onStopMetrics;   // Called when server sends stop message

private:
    void runEventLoop();
    void handleMessage(const std::string& message);

    boost::asio::io_context ioc;
    boost::asio::ip::tcp::resolver resolver{ioc};
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};

    // Use variant to handle both SSL and non-SSL connections
    std::unique_ptr<boost::beast::websocket::stream<boost::asio::ssl::stream<boost::beast::tcp_stream>>> ssl_ws;
    std::unique_ptr<boost::beast::websocket::stream<boost::beast::tcp_stream>> plain_ws;
    bool use_ssl_connection = true;

    std::thread ioThread;
    std::atomic<bool> shouldStop{false};
};

