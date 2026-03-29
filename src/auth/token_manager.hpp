#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <functional>
#include "core/interfaces.hpp"

// Forward-declare gRPC stubs to avoid heavy includes in headers
namespace grpc { class Channel; }
namespace proto::tradeapi::v1 { class AuthService; }

namespace finam::auth {

struct TokenManagerConfig {
    std::string endpoint;              // "api.finam.ru:443"
    std::string secret_token;          // from $FINAM_SECRET_TOKEN
    std::chrono::seconds rpc_timeout{10};
};

// Thread-safe JWT manager.
// - Obtains JWT via AuthService.Auth(secret)
// - Exposes jwt() — lock-free atomic read, safe from any thread
// - Runs SubscribeJwtRenewal stream in background thread
// - Fetches account_ids via TokenDetails after initial auth
//
// Lifetime: create once, pass by shared_ptr everywhere.
class TokenManager {
public:
    explicit TokenManager(TokenManagerConfig cfg);
    ~TokenManager();

    // Non-copyable, non-movable (owns background thread)
    TokenManager(const TokenManager&)            = delete;
    TokenManager& operator=(const TokenManager&) = delete;

    // Blocking: connects, authenticates, fetches account_ids.
    // Must be called before jwt() or account_ids().
    [[nodiscard]] Result<void> init();

    // Lock-free, safe to call from hot path.
    [[nodiscard]] std::string jwt() const;

    // Available after init()
    [[nodiscard]] const std::vector<std::string>& account_ids() const noexcept;

    // Injects Authorization header into gRPC metadata.
    // Pass to CallCredentials or intercept manually.
    [[nodiscard]] std::string auth_header() const;

    // Stops the renewal background thread.
    void shutdown() noexcept;

private:
    Result<void> do_auth();
    Result<void> fetch_account_ids();
    void         run_renewal_stream();   // runs in thread_

    TokenManagerConfig              cfg_;
    std::shared_ptr<grpc::Channel>  channel_;

    // Atomic string for lock-free JWT reads from strategy/order threads
    // We store as shared_ptr<const string> behind atomic to avoid data races
    std::atomic<std::shared_ptr<const std::string>> jwt_;

    std::vector<std::string> account_ids_;
    std::atomic<bool>        stop_{false};
    std::thread              renewal_thread_;
};

} // namespace finam::auth
