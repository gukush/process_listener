#include "websocket_listener.hpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <regex>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

WebSocketListener::WebSocketListener() {
    // Configure SSL context to accept self-signed certificates
    ssl_ctx.set_verify_mode(ssl::verify_none);
    ssl_ctx.set_options(ssl::context::default_workarounds |
                       ssl::context::no_sslv2 |
                       ssl::context::no_sslv3 |
                       ssl::context::single_dh_use);
}

WebSocketListener::~WebSocketListener() {
    disconnect();
}

// Simple URL parser function
struct ParsedURL {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
};

ParsedURL parseURL(const std::string& url) {
    ParsedURL result;

    // Regex to parse URL: (scheme)://(host):(port)(path)
    std::regex url_regex(R"(^(wss?):\/\/([^:\/]+)(?::(\d+))?(\/.*)?$)");
    std::smatch matches;

    if (std::regex_match(url, matches, url_regex)) {
        result.scheme = matches[1].str();
        result.host = matches[2].str();
        result.port = matches[3].str();
        result.path = matches[4].str();

        // Set default ports
        if (result.port.empty()) {
            result.port = (result.scheme == "wss") ? "443" : "80";
        }

        // Set default path
        if (result.path.empty()) {
            result.path = "/";
        }
    } else {
        throw std::invalid_argument("Invalid URL format");
    }

    return result;
}

bool WebSocketListener::connect(const std::string& url) {
    try {
        // Parse the WebSocket URL
        ParsedURL parsed_url = parseURL(url);

        use_ssl_connection = (parsed_url.scheme == "wss");

        std::cout << "[WS-Listener] Connecting to " << parsed_url.scheme << "://"
                  << parsed_url.host << ":" << parsed_url.port << parsed_url.path << std::endl;

        // Resolve hostname with better error handling
        boost::system::error_code ec;
        auto results = resolver.resolve(parsed_url.host, parsed_url.port, ec);

        if (ec) {
            std::cerr << "[WS-Listener] DNS resolution failed for " << parsed_url.host
                      << ":" << parsed_url.port << " - " << ec.message() << std::endl;

            // Try fallback for localhost addresses
            if (parsed_url.host == "127.0.0.1" || parsed_url.host == "localhost") {
                std::cerr << "[WS-Listener] Trying fallback resolution for localhost..." << std::endl;
                results = resolver.resolve(tcp::v4(), parsed_url.host, parsed_url.port, ec);
                if (ec) {
                    std::cerr << "[WS-Listener] Fallback DNS resolution also failed: " << ec.message() << std::endl;
                    return false;
                }
            } else {
                return false;
            }
        }

        if (use_ssl_connection) {
            // SSL WebSocket connection
            ssl_ws = std::make_unique<boost::beast::websocket::stream<boost::asio::ssl::stream<boost::beast::tcp_stream>>>(ioc, ssl_ctx);

            // Get the underlying socket
            auto& socket = beast::get_lowest_layer(*ssl_ws);

            // Make the connection on the IP address we get from a lookup
            socket.connect(results);

            // Update the host string for SNI
            std::string hostWithPort = parsed_url.host + ':' + parsed_url.port;

            // Set SNI Hostname (many hosts need this to handshake successfully)
            if (!SSL_set_tlsext_host_name(ssl_ws->next_layer().native_handle(), parsed_url.host.c_str())) {
                beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
                throw beast::system_error{ec};
            }

            // Perform the SSL handshake
            ssl_ws->next_layer().handshake(ssl::stream_base::client);

            // Set a decorator to change the User-Agent of the handshake
            ssl_ws->set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(beast::http::field::user_agent, "Metrics-Listener/1.0");
                }));

            // Connect to the WebSocket endpoint
            ssl_ws->handshake(hostWithPort, parsed_url.path);

            std::cout << "[WS-Listener] Connected to SSL WebSocket endpoint: " << parsed_url.path << std::endl;
        } else {
            // Plain WebSocket connection
            plain_ws = std::make_unique<boost::beast::websocket::stream<boost::beast::tcp_stream>>(ioc);

            // Get the underlying socket
            auto& socket = plain_ws->next_layer();

            // Make the connection on the IP address we get from a lookup
            socket.connect(results);

            // Set a decorator to change the User-Agent of the handshake
            plain_ws->set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(beast::http::field::user_agent, "Metrics-Listener/1.0");
                }));

            // Connect to the WebSocket endpoint
            plain_ws->handshake(parsed_url.host, parsed_url.path);

            std::cout << "[WS-Listener] Connected to plain WebSocket endpoint: " << parsed_url.path << std::endl;
        }

        // Start the event loop in a separate thread
        shouldStop = false;
        ioThread = std::thread(&WebSocketListener::runEventLoop, this);

        return true;

    } catch (std::exception const& e) {
        std::cerr << "[WS-Listener] WebSocket connection error: " << e.what() << std::endl;
        return false;
    }
}

void WebSocketListener::disconnect() {
    shouldStop = true;

    if (use_ssl_connection && ssl_ws && ssl_ws->is_open()) {
        try {
            ssl_ws->close(websocket::close_code::normal);
        } catch (...) {
            // Ignore errors during close
        }
    } else if (!use_ssl_connection && plain_ws && plain_ws->is_open()) {
        try {
            plain_ws->close(websocket::close_code::normal);
        } catch (...) {
            // Ignore errors during close
        }
    }

    if (ioThread.joinable()) {
        ioThread.join();
    }
}

bool WebSocketListener::isConnected() const {
    if (use_ssl_connection) {
        return ssl_ws && ssl_ws->is_open();
    } else {
        return plain_ws && plain_ws->is_open();
    }
}

void WebSocketListener::runEventLoop() {
    beast::flat_buffer buffer;

    while (!shouldStop && isConnected()) {
        try {
            // Read a message
            if (use_ssl_connection && ssl_ws) {
                ssl_ws->read(buffer);
            } else if (!use_ssl_connection && plain_ws) {
                plain_ws->read(buffer);
            } else {
                break;
            }

            // Convert to string
            std::string messageStr = beast::buffers_to_string(buffer.data());
            buffer.clear();

            // Handle the message
            handleMessage(messageStr);

        } catch (beast::system_error const& se) {
            if (se.code() != websocket::error::closed) {
                std::cerr << "[WS-Listener] WebSocket read error: " << se.code().message() << std::endl;
            }
            break;
        } catch (std::exception const& e) {
            std::cerr << "[WS-Listener] WebSocket event loop error: " << e.what() << std::endl;
            break;
        }
    }

    std::cout << "[WS-Listener] WebSocket event loop ended" << std::endl;
}

void WebSocketListener::handleMessage(const std::string& message) {
    try {
        json messageJson = json::parse(message);
        std::string eventType = messageJson.value("type", "");
        json eventData = messageJson.value("data", json::object());

        std::cout << "[WS-Listener] Received message: " << eventType << std::endl;

        // Handle different event types
        if (eventType == "metrics:start") {
            std::cout << "[WS-Listener] Starting metrics collection..." << std::endl;
            if (onStartMetrics) {
                onStartMetrics();
            }
        } else if (eventType == "metrics:stop") {
            std::cout << "[WS-Listener] Stopping metrics collection..." << std::endl;
            if (onStopMetrics) {
                onStopMetrics();
            }
        } else {
            std::cout << "[WS-Listener] Unknown event type: " << eventType << std::endl;
        }

    } catch (json::parse_error const& e) {
        std::cerr << "[WS-Listener] JSON parse error: " << e.what() << std::endl;
        std::cerr << "[WS-Listener] Raw message: " << message << std::endl;
    }
}