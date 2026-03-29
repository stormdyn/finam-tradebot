#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>
#include <vector>
#include "core/interfaces.hpp"

namespace grpc { class Channel; }

namespace finam::auth {

struct Config {
    std::string endpoint;    // "api.finam.ru:443"
    std::string secret;      // $FINAM_SECRET_TOKEN — никогда не логировать
    std::chrono::seconds rpc_timeout{10};
};

// JWT-менеджер для Finam Trade API v2 (новый API, tradeapi.finam.ru).
//
// Поток жизни:
//   1. init()          — Auth(secret) → JWT, TokenDetails(jwt) → account_ids
//   2. jwt()           — lock-free чтение из любого потока (hot path)
//   3. channel()       — gRPC канал с встроенным JWT interceptor
//   4. ~TokenManager() — останавливает renewal thread
//
// Thread-safety: jwt() — атомарный; channel() — immutable после init().
class TokenManager {
public:
    explicit TokenManager(Config cfg);
    ~TokenManager();

    TokenManager(const TokenManager&)            = delete;
    TokenManager& operator=(const TokenManager&) = delete;

    // Блокирующий вызов. Обязателен перед всем остальным.
    [[nodiscard]] Result<void> init();

    // Lock-free, безопасен из любого потока включая стратегию.
    [[nodiscard]] std::string jwt() const noexcept;

    // gRPC канал — передавать в Stub конструкторы.
    // JWT interceptor встроен — не нужно вручную добавлять заголовки.
    [[nodiscard]] std::shared_ptr<grpc::Channel> channel() const noexcept;

    // Список торговых счетов — доступен после init().
    [[nodiscard]] const std::vector<std::string>& account_ids() const noexcept;

    // Первый счёт — удобный хелпер.
    [[nodiscard]] std::string_view primary_account_id() const;

    void shutdown() noexcept;

private:
    [[nodiscard]] Result<void> do_auth();
    [[nodiscard]] Result<void> fetch_account_ids();
    void run_renewal_stream();  // фоновый поток

    void store_jwt(std::string jwt);

    Config cfg_;
    std::shared_ptr<grpc::Channel> channel_;

    // shared_ptr<const string> за atomic — lock-free читатели, нет data race
    std::atomic<std::shared_ptr<const std::string>> jwt_;

    std::vector<std::string> account_ids_;
    std::atomic<bool>        stop_{false};
    std::thread              renewal_thread_;
};

} // namespace finam::auth