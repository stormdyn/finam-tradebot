#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <spdlog/spdlog.h>

namespace finam::core {

// ── HealthServer ─────────────────────────────────────────────────────────────────
//
// Минимальный HTTP/1.1 сервер без зависимостей (только POSIX sockets).
// Отвечает на GET /health и GET /metrics через callback.
//
// Docker: HEALTHCHECK --interval=10s CMD curl -f http://localhost:8080/health
// Kubernetes: livenessProbe httpGet path=/health port=8080
//
// Трейдофф: не используем beast/httplib — избегаем депенденсий,
// по цене ещё одного thread + ручного parse HTTP.
// Достаточно для health-check в проде (не latency-critical path).

struct HealthStatus {
    bool   ok{true};
    std::string active_symbol;
    double daily_pnl{0.0};
    double liquid_value{0.0};
    bool   circuit_breaker{false};
};

class HealthServer {
public:
    using StatusProvider = std::function<HealthStatus()>;

    explicit HealthServer(uint16_t port, StatusProvider provider)
        : port_(port), provider_(std::move(provider)) {}

    ~HealthServer() { stop(); }

    void start() {
        stop_.store(false);
        thread_ = std::thread(&HealthServer::serve_loop, this);
        spdlog::info("[Health] HTTP server started on port {}", port_);
    }

    void stop() noexcept {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

private:
    void serve_loop() {
        const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) { spdlog::error("[Health] socket() failed"); return; }

        const int opt = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            spdlog::error("[Health] bind() failed on port {}", port_);
            ::close(server_fd);
            return;
        }
        ::listen(server_fd, 4);

        // Неблокирующий accept через select() с таймаутом —
        // чтобы срабатывал на stop_
        while (!stop_.load(std::memory_order_acquire)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(server_fd, &fds);
            timeval tv{1, 0};  // 1 second timeout
            if (::select(server_fd + 1, &fds, nullptr, nullptr, &tv) <= 0)
                continue;

            const int client_fd = ::accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) continue;
            handle_request(client_fd);
            ::close(client_fd);
        }
        ::close(server_fd);
    }

    void handle_request(int fd) {
        char buf[256]{};
        ::recv(fd, buf, sizeof(buf) - 1, 0);
        const std::string_view req{buf};

        const bool is_metrics = req.find("GET /metrics") != std::string_view::npos;
        const bool is_health  = req.find("GET /health")  != std::string_view::npos;
        if (!is_health && !is_metrics) return;

        const auto st = provider_();
        const std::string body = build_body(st, is_metrics);
        const std::string http_code = st.ok ? "200 OK" : "503 Service Unavailable";
        const std::string resp =
            "HTTP/1.1 " + http_code + "\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
    }

    static std::string build_body(const HealthStatus& st, bool metrics) {
        if (!metrics) {
            return st.ok ? "ok" : "circuit_breaker_tripped";
        }
        // Prometheus text format
        return
            "# HELP tradebot_ok Bot health (1=ok, 0=tripped)\n"
            "tradebot_ok " + std::to_string(st.ok ? 1 : 0) + "\n"
            "# HELP tradebot_daily_pnl Daily PnL in ticks\n"
            "tradebot_daily_pnl " + std::to_string(st.daily_pnl) + "\n"
            "# HELP tradebot_liquid_value Net liquidation value\n"
            "tradebot_liquid_value " + std::to_string(st.liquid_value) + "\n"
            "# HELP tradebot_circuit_breaker Circuit breaker (1=tripped)\n"
            "tradebot_circuit_breaker " + std::to_string(st.circuit_breaker ? 1 : 0) + "\n";
    }

    uint16_t       port_;
    StatusProvider provider_;
    std::thread    thread_;
    std::atomic<bool> stop_{false};
};

} // namespace finam::core
