#include "websocket_proxy.hpp"
#include "orchestrator.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <iostream>
#include <atomic>

namespace unified_monitor {

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;
namespace beast = boost::beast;
namespace ssl = boost::asio::ssl;  // Add SSL namespace alias

struct BeastWebSocketProxy::Impl {
    explicit Impl(ChunkTracker* tr, EnhancedMessageInterceptor* in)
        : tracker(tr), interceptor(in), ioc(1) {}

    ChunkTracker* tracker;
    EnhancedMessageInterceptor* interceptor;

    boost::asio::io_context ioc;
    std::unique_ptr<tcp::acceptor> acceptor;
    std::string upstream_host;
    uint16_t upstream_port = 0;

    std::atomic<bool> running{false};

    struct ProxySession {
        boost::asio::io_context& ioc;
        tcp::socket client_socket;
        websocket::stream<tcp::socket> client_ws;

        tcp::resolver resolver;
        std::unique_ptr<websocket::stream<tcp::socket>> upstream_ws;
        std::unique_ptr<websocket::stream<ssl::stream<tcp::socket>>> upstream_ws_ssl;

        EnhancedMessageInterceptor* interceptor;

        std::string upstream_host;
        uint16_t upstream_port;
        bool use_ssl;

        ProxySession(boost::asio::io_context& ioc_,
                     tcp::socket&& sock,
                     const std::string& host,
                     uint16_t port,
                     EnhancedMessageInterceptor* intr)
            : ioc(ioc_),
              client_socket(std::move(sock)),
              client_ws(std::move(client_socket)),
              resolver(ioc_),
              interceptor(intr),
              upstream_host(host),
              upstream_port(port),
              use_ssl(host.find("wss://") == 0 || port == 443) {
            
            if (use_ssl) {
                ssl::context ctx{ssl::context::tlsv12_client};
                ctx.set_default_verify_paths();
                ctx.set_verify_mode(ssl::verify_none); // For testing - should be verify_peer in production
                upstream_ws_ssl = std::make_unique<websocket::stream<ssl::stream<tcp::socket>>>(ioc_, ctx);
            } else {
                upstream_ws = std::make_unique<websocket::stream<tcp::socket>>(ioc_);
            }
        }

        void run() {
            beast::error_code ec;
            client_ws.accept(ec);
            if (ec) { std::cerr << "[proxy] client accept error: " << ec.message() << "\n"; return; }

            auto results = resolver.resolve(upstream_host, std::to_string(upstream_port), ec);
            if (ec) { std::cerr << "[proxy] resolve error: " << ec.message() << "\n"; return; }

            // Set read/write timeouts for client
            client_ws.set_option(websocket::stream_base::timeout{
                std::chrono::seconds(30),
                std::chrono::seconds(30),
                true
            });

            // Determine path based on port - Socket.IO uses root, native uses /ws-native
            std::string path = (upstream_port == 3000) ? "/" : "/ws-native";

            if (use_ssl) {
                // SSL WebSocket connection
                auto ep = boost::asio::connect(upstream_ws_ssl->next_layer().next_layer(), results, ec);
                if (ec) { std::cerr << "[proxy] SSL connect error: " << ec.message() << "\n"; return; }
                
                upstream_ws_ssl->next_layer().handshake(ssl::stream_base::client, ec);
                if (ec) { std::cerr << "[proxy] SSL handshake error: " << ec.message() << "\n"; return; }
                
                upstream_ws_ssl->set_option(websocket::stream_base::timeout{
                    std::chrono::seconds(30),
                    std::chrono::seconds(30),
                    true
                });
                
                upstream_ws_ssl->handshake(upstream_host, path, ec);
                if (ec) { std::cerr << "[proxy] SSL WS handshake error: " << ec.message() << "\n"; return; }
            } else {
                // Regular WebSocket connection
                auto ep = boost::asio::connect(upstream_ws->next_layer(), results, ec);
                if (ec) { std::cerr << "[proxy] connect error: " << ec.message() << "\n"; return; }

                upstream_ws->set_option(websocket::stream_base::timeout{
                    std::chrono::seconds(30),
                    std::chrono::seconds(30),
                    true
                });
                
                upstream_ws->handshake(upstream_host, path, ec);
                if (ec) { std::cerr << "[proxy] handshake error: " << ec.message() << "\n"; return; }
            }

            std::thread t_cs([this]{ pump_client_to_server(); });
            std::thread t_sc([this]{ pump_server_to_client(); });

            t_cs.join();
            t_sc.join();

            beast::error_code ignore;
            if (use_ssl) {
                upstream_ws_ssl->close(websocket::close_code::normal, ignore);
            } else {
                upstream_ws->close(websocket::close_code::normal, ignore);
            }
            client_ws.close(websocket::close_code::normal, ignore);
        }

        void intercept_if_json(const std::string& text, bool from_server) {
            try {
                auto j = nlohmann::json::parse(text, nullptr, false);
                if (j.is_discarded()) return;

                auto call = [&](const std::string& ev, const nlohmann::json& payload) {
                    if (!interceptor) return;
                    if (ev == "task:init") interceptor->onTaskInit(payload);
                    else if (ev == "chunk:assign") interceptor->onChunkAssign(payload);
                    else if (ev == "chunk:result") interceptor->onChunkResult(payload);
                };

                // Handle Socket.IO format (port 3000)
                if (upstream_port == 3000) {
                    if (j.is_array() && j.size() >= 2 && j[0].is_string()) {
                        call(j[0].get<std::string>(), j[1]);
                    }
                }
                // Handle native WebSocket format (port 3001)
                else if (upstream_port == 3001) {
                    if (j.is_object()) {
                        if (j.contains("type") && j.contains("data")) {
                            std::string type = j["type"].get<std::string>();
                            const auto& data = j["data"];
                            
                            // Map native message types to chunk events
                            if (type == "workload:new") {
                                // This is a task initialization
                                if (data.contains("taskId")) {
                                    nlohmann::json taskInit = {
                                        {"taskId", data["taskId"]},
                                        {"strategyId", data.value("framework", "")}
                                    };
                                    call("task:init", taskInit);
                                }
                            } else if (type == "workload:chunk") {
                                // This is a chunk assignment
                                if (data.contains("chunkId")) {
                                    call("chunk:assign", data);
                                }
                            } else if (type == "workload:chunk_done_enhanced") {
                                // This is a chunk result
                                if (data.contains("chunkId")) {
                                    call("chunk:result", data);
                                }
                            }
                        } else if (j.contains("chunkId") && j.contains("status") && from_server) {
                            call("chunk:result", j);
                        }
                    }
                }
            } catch (...) {
                // ignore
            }
        }

        void pump_client_to_server() {
            beast::flat_buffer buf;
            beast::error_code ec;

            while (true) {
                auto bytes = client_ws.read(buf, ec);
                if (ec) break;
                bool is_text = client_ws.got_text();
                if (is_text) {
                    std::string msg = beast::buffers_to_string(buf.data());
                    intercept_if_json(msg, /*from_server=*/false);
                    if (use_ssl) {
                        upstream_ws_ssl->text(true);
                        upstream_ws_ssl->write(boost::asio::buffer(msg), ec);
                    } else {
                        upstream_ws->text(true);
                        upstream_ws->write(boost::asio::buffer(msg), ec);
                    }
                } else {
                    if (use_ssl) {
                        upstream_ws_ssl->binary(true);
                        upstream_ws_ssl->write(buf.data(), ec);
                    } else {
                        upstream_ws->binary(true);
                        upstream_ws->write(buf.data(), ec);
                    }
                }
                buf.consume(bytes);
                if (ec) break;
            }
        }

        void pump_server_to_client() {
            beast::flat_buffer buf;
            beast::error_code ec;

            while (true) {
                auto bytes = use_ssl ? upstream_ws_ssl->read(buf, ec) : upstream_ws->read(buf, ec);
                if (ec) break;
                bool is_text = use_ssl ? upstream_ws_ssl->got_text() : upstream_ws->got_text();
                if (is_text) {
                    std::string msg = beast::buffers_to_string(buf.data());
                    intercept_if_json(msg, /*from_server=*/true);
                    client_ws.text(true);
                    client_ws.write(boost::asio::buffer(msg), ec);
                } else {
                    client_ws.binary(true);
                    client_ws.write(buf.data(), ec);
                }
                buf.consume(bytes);
                if (ec) break;
            }
        }
    };

    void do_accept() {
        acceptor->async_accept(
            boost::asio::make_strand(ioc),
            [this](beast::error_code ec, tcp::socket socket) {
                if (!running.load()) return;

                if (!ec) {
                    std::thread([this, sock = std::move(socket)]() mutable {
                        try {
                            ProxySession sess(ioc, std::move(sock), upstream_host, upstream_port, interceptor);
                            sess.run();
                        } catch (const std::exception& e) {
                            std::cerr << "[proxy] session crashed: " << e.what() << "\n";
                        }
                    }).detach();
                }
                do_accept();
            });
    }
};

BeastWebSocketProxy::BeastWebSocketProxy(ChunkTracker* tracker, EnhancedMessageInterceptor* interceptor)
    : impl_(std::make_unique<Impl>(tracker, interceptor)) {}

BeastWebSocketProxy::~BeastWebSocketProxy() { stop(); }

bool BeastWebSocketProxy::start(uint16_t listen_port, const std::string& upstream_host, uint16_t upstream_port) {
    if (impl_->running.exchange(true)) return true;

    beast::error_code ec;

    impl_->acceptor = std::make_unique<tcp::acceptor>(impl_->ioc);
    tcp::endpoint endpoint{tcp::v4(), listen_port};

    impl_->acceptor->open(endpoint.protocol(), ec);
    if (ec) { std::cerr << "[proxy] acceptor open: " << ec.message() << "\n"; return false; }

    impl_->acceptor->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) { std::cerr << "[proxy] set_option: " << ec.message() << "\n"; return false; }

    impl_->acceptor->bind(endpoint, ec);
    if (ec) { std::cerr << "[proxy] bind: " << ec.message() << "\n"; return false; }

    impl_->acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) { std::cerr << "[proxy] listen: " << ec.message() << "\n"; return false; }

    impl_->upstream_host = upstream_host;
    impl_->upstream_port = upstream_port;

    impl_->do_accept();

    io_thread_ = std::thread([this, listen_port, upstream_host, upstream_port]() {
        std::cout << "[proxy] listening on :" << listen_port
                  << " -> " << upstream_host << ":" << upstream_port << std::endl;
        impl_->ioc.run();
    });

    return true;
}

void BeastWebSocketProxy::stop() {
    if (!impl_->running.exchange(false)) return;
    beast::error_code ec;
    if (impl_->acceptor) {
        impl_->acceptor->cancel(ec);
        impl_->acceptor->close(ec);
    }
    impl_->ioc.stop();
    if (io_thread_.joinable()) io_thread_.join();
}

} // namespace unified_monitor
