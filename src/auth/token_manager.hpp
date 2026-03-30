#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <grpcpp/grpcpp.h>
#include "core/interfaces.hpp"

namespace finam::auth {

struct TokenManagerConfig {
    std::string endpoint{"api.finam.ru:443"};
    bool        use_tls{true};
};

class TokenManager {
public:
    using Config = TokenManagerConfig;

    explicit TokenManager(Config cfg);
    ~TokenManager();

    // Auth(secret) → JWT, TokenDetails → account_ids,
    // запуск фонового SubscribeJwtRenewal стрима
    Result<void> init(std::string_view secret);

    // Текущий JWT (атомарно, lock-free)
    [[nodiscard]] std::string                    jwt()                const noexcept;
    [[nodiscard]] std::shared_ptr<grpc::Channel> channel()            const noexcept;
    [[nodiscard]] const std::vector<std::string>& account_ids()       const noexcept;
    [[nodiscard]] std::string_view               primary_account_id() const;

    void shutdown() noexcept;

private:
    void store_jwt(std::string jwt);
    void run_renewal_stream(std::string secret);

    Config cfg_;
    std::shared_ptr<grpc::Channel> channel_;

    // Атомарный shared_ptr — JWT читается из hot path без мьютекса
    std::atomic<std::shared_ptr<const std::string>> jwt_;

    std::vector<std::string> account_ids_;
    std::atomic<bool>        stop_{false};
    std::thread              renewal_thread_;
};

} // namespace finam::auth
