#include "websocket_proxy.hpp"
#include "orchestrator.cpp" // declarations for ChunkTracker (pragma once)

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>

#include <iostream>
#include <atomic>

namespace unified_monitor {

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;
namespace beast = boost::beast;

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

    // One proxy session per client
    struct ProxySession {
        boost::asio::io_context& ioc;
        tcp::socket client_socket;
        websocket::stream<tcp::socket> client_ws;

        tcp::resolver resolver;
        websocket::stream<tcp::socket> upstream_ws;

        EnhancedMessageInterceptor* interceptor;

        std::string upstream_host;
        uint16_t upstream_port;

        std::vector<char> buffer;

        ProxySession(boost::asio::io_context& ioc_,
                     tcp::socket&& sock,
                     const std::string& host,
                     uint16_t port,
                     EnhancedMessageInterceptor* intr)
            : ioc(ioc_),
              client_socket(std::move(sock)),
              client_ws(std::move(client_socket)),
              resolver(ioc_),
              upstream_ws(ioc_),
              interceptor(intr),
              upstream_host(host),
              upstream_port(port) {}

        void run() {
            beast::error_code ec;
            client_ws.accept(ec);
            if (ec) { std::cerr << "[proxy] client accept error: " << ec.message() << "\n"; return; }

            auto results = resolver.resolve(upstream_host, std::to_string(upstream_port), ec);
            if (ec) { std::cerr << "[proxy] resolve error: " << ec.message() << "\n"; return; }

            auto ep = boost::asio::connect(upstream_ws.next_layer(), results, ec);
            if (ec) { std::cerr << "[proxy] connect error: " << ec.message() << "\n"; return; }

            upstream_ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
            upstream_ws.handshake(upstream_host, "/", ec);
            if (ec) { std::cerr << "[proxy] handshake error: " << ec.message() << "\n"; return; }

            // Two threads: C->S and S->C pumps.
            std::thread t_cs([this]{ pump_client_to_server(); });
            std::thread t_sc([this]{ pump_server_to_client(); });

            t_cs.join();
            t_sc.join();

            beast::error_code ignore;
            upstream_ws.close(websocket::close_code::normal, ignore);
            client_ws.close(websocket::close_code::normal, ignore);
        }

        void intercept_if_json(const std::string& text, bool from_server) {
            try {
                auto j = nlohmann::json::parse(text, nullptr, false);
                if (j.is_discarded()) return;

                // Accept common Socket.IO-like envelopes:
                // 1) { "event": "...", "data": {...} }
                // 2) ["event", {...}]
                // 3) bare message with known keys
                auto call = [&](const std::string& ev, const nlohmann::json& payload) {
                    if (!interceptor) return;
                    if (ev == "task:init") interceptor->onTaskInit(payload);
                    else if (ev == "chunk:assign") interceptor->onChunkAssign(payload);
                    else if (ev == "chunk:result") interceptor->onChunkResult(payload);
                };

                if (j.is_object()) {
                    if (j.contains("event")) {
                        std::string ev = j["event"].get<std::string>();
                        const auto& data = j.contains("data") ? j["data"] : j;
                        call(ev, data);
                    } else if (j.contains("chunkId") && j.contains("status") && from_server) {
                        call("chunk:result", j);
                    }
                } else if (j.is_array() && j.size() >= 2 && j[0].is_string()) {
                    call(j[0].get<std::string>(), j[1]);
                }
            } catch (...) {
                // ignore non-json
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
                    upstream_ws.text(true);
                    upstream_ws.write(boost::asio::buffer(msg), ec);
                } else {
                    upstream_ws.binary(true);
                    upstream_ws.write(buf.data(), ec);
                }
                buf.consume(bytes);
                if (ec) break;
            }
        }

        void pump_server_to_client() {
            beast::flat_buffer buf;
            beast::error_code ec;

            while (true) {
                auto bytes = upstream_ws.read(buf, ec);
                if (ec) break;
                bool is_text = upstream_ws.got_text();
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
    auto address = boost::asio::ip::make_address("0.0.0.0", ec);
    if (ec) { std::cerr << "[proxy] bad listen address: " << ec.message() << "\n"; return false; }

    impl_->acceptor = std::make_unique<tcp::acceptor>(impl_->ioc, tcp::endpoint{address, listen_port}, ec);
    if (ec) { std::cerr << "[proxy] acceptor error: " << ec.message() << "\n"; return false; }

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
