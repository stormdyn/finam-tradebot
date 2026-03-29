#include "token_manager.hpp"

#include <spdlog/spdlog.h>
#include <grpcpp/grpcpp.h>

// Generated proto stubs — will exist after proto submodule compile
// #include <proto/tradeapi/v1/auth.grpc.pb.h>
// Uncomment when proto targets are available.

namespace finam::auth {

TokenManager::TokenManager(TokenManagerConfig cfg)
    : cfg_(std::move(cfg))
    , jwt_(std::make_shared<const std::string>()) {}

TokenManager::~TokenManager() {
    shutdown();
}

Result<void> TokenManager::init() {
    spdlog::info("[TokenManager] connecting to {}", cfg_.endpoint);

    // TLS channel — production mandatory
    auto creds = grpc::SslCredentials(grpc::SslCredentialsOptions{});
    channel_ = grpc::CreateChannel(cfg_.endpoint, creds);

    if (auto r = do_auth(); !r) return r;
    if (auto r = fetch_account_ids(); !r) return r;

    // Start background renewal BEFORE returning
    renewal_thread_ = std::thread(&TokenManager::run_renewal_stream, this);
    spdlog::info("[TokenManager] init OK, {} account(s)", account_ids_.size());
    return {};
}

std::string TokenManager::jwt() const {
    // atomic load — no lock, safe from any thread
    return *jwt_.load(std::memory_order_acquire);
}

const std::vector<std::string>& TokenManager::account_ids() const noexcept {
    return account_ids_;
}

std::string TokenManager::auth_header() const {
    return "Bearer " + jwt();
}

void TokenManager::shutdown() noexcept {
    stop_.store(true, std::memory_order_release);
    if (renewal_thread_.joinable()) renewal_thread_.join();
}

// ─── Private ────────────────────────────────────────────────────────────────

Result<void> TokenManager::do_auth() {
    // TODO: uncomment when proto stubs are compiled
    // proto::tradeapi::v1::AuthService::Stub stub(channel_);
    // grpc::ClientContext ctx;
    // ctx.set_deadline(std::chrono::system_clock::now() + cfg_.rpc_timeout);
    //
    // AuthRequest req;
    // req.set_token(cfg_.secret_token);   // NEVER log this
    // AuthResponse resp;
    // if (auto s = stub.Auth(&ctx, req, &resp); !s.ok()) {
    //     return std::unexpected(Error{ErrorCode::AuthFailed, s.error_message()});
    // }
    // jwt_.store(std::make_shared<const std::string>(resp.token()),
    //            std::memory_order_release);
    // spdlog::info("[TokenManager] JWT obtained");

    spdlog::warn("[TokenManager] do_auth: proto stubs not yet generated (stub)");
    // Placeholder JWT for compile testing
    jwt_.store(std::make_shared<const std::string>("stub-jwt"),
               std::memory_order_release);
    return {};
}

Result<void> TokenManager::fetch_account_ids() {
    // TODO: uncomment when proto stubs are compiled
    // proto::tradeapi::v1::AuthService::Stub stub(channel_);
    // grpc::ClientContext ctx;
    // ctx.set_deadline(std::chrono::system_clock::now() + cfg_.rpc_timeout);
    //
    // TokenDetailsRequest req;
    // req.set_token(jwt());
    // TokenDetailsResponse resp;
    // if (auto s = stub.TokenDetails(&ctx, req, &resp); !s.ok()) {
    //     return std::unexpected(Error{ErrorCode::AuthFailed, s.error_message()});
    // }
    // for (const auto& id : resp.account_ids()) account_ids_.push_back(id);

    spdlog::warn("[TokenManager] fetch_account_ids: proto stubs not yet generated (stub)");
    account_ids_.push_back("stub-account-001");
    return {};
}

void TokenManager::run_renewal_stream() {
    // Stream TTL = 86400s per API spec; reconnect is expected daily.
    // On maintenance window (05:00-06:15 MSK) the stream will break;
    // we retry with exponential backoff.
    using namespace std::chrono_literals;
    constexpr auto kMaxBackoff = 60s;
    auto backoff = 1s;

    while (!stop_.load(std::memory_order_acquire)) {
        spdlog::debug("[TokenManager] starting SubscribeJwtRenewal stream");

        // TODO: uncomment when proto stubs are compiled
        // proto::tradeapi::v1::AuthService::Stub stub(channel_);
        // grpc::ClientContext ctx;
        // SubscribeJwtRenewalRequest req;
        // req.set_token(cfg_.secret_token);  // NEVER log this
        // auto reader = stub.SubscribeJwtRenewal(&ctx, req);
        //
        // SubscribeJwtRenewalResponse resp;
        // while (!stop_ && reader->Read(&resp)) {
        //     jwt_.store(std::make_shared<const std::string>(resp.token()),
        //                std::memory_order_release);
        //     spdlog::info("[TokenManager] JWT refreshed");
        //     backoff = 1s;  // reset on success
        // }
        // auto status = reader->Finish();
        // if (!stop_) {
        //     spdlog::warn("[TokenManager] renewal stream ended: {}",
        //                  status.error_message());
        // }

        // Stub: just sleep
        if (!stop_.load(std::memory_order_acquire))
            std::this_thread::sleep_for(backoff);

        backoff = std::min(backoff * 2, kMaxBackoff);
    }
    spdlog::info("[TokenManager] renewal thread stopped");
}

} // namespace finam::auth
