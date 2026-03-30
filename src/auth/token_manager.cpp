#include "token_manager.hpp"
#include <stdexcept>
#include "core/grpc_fmt.hpp"       // ПЕРЕД spdlog!
#include <spdlog/spdlog.h>
#include "grpc/tradeapi/v1/auth/auth_service.grpc.pb.h"

namespace finam::auth {

using AuthStub = ::grpc::tradeapi::v1::auth::AuthService::Stub;

TokenManager::TokenManager(Config cfg)
    : cfg_(std::move(cfg))
    , jwt_(std::make_shared<const std::string>())
{}

TokenManager::~TokenManager() { shutdown(); }

// ── Создание канала ───────────────────────────────────────────────────────

static std::shared_ptr<grpc::Channel> make_channel(const TokenManagerConfig& cfg) {
    if (cfg.use_tls)
        return grpc::CreateChannel(cfg.endpoint, grpc::SslCredentials({}));
    return grpc::CreateChannel(cfg.endpoint, grpc::InsecureChannelCredentials());
}

// ── init ───────────────────────────────────────────────────────────────────

Result<void> TokenManager::init(std::string_view secret) {
    channel_ = make_channel(cfg_);
    auto stub = AuthStub(channel_);

    // 1. Auth(secret) → JWT
    ::grpc::tradeapi::v1::auth::AuthRequest req;
    req.set_secret(std::string(secret));
    ::grpc::tradeapi::v1::auth::AuthResponse resp;
    grpc::ClientContext ctx;

    const auto status = stub.Auth(&ctx, req, &resp);
    if (!status.ok()) {
        spdlog::error("[Auth] Auth() failed: code={} msg={}",
            status.error_code(), status.error_message());
        return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    }
    store_jwt(resp.token());
    spdlog::info("[Auth] JWT obtained");

    // 2. TokenDetails(jwt) → account_ids
    ::grpc::tradeapi::v1::auth::TokenDetailsRequest det_req;
    det_req.set_token(resp.token());
    ::grpc::tradeapi::v1::auth::TokenDetailsResponse det_resp;
    grpc::ClientContext det_ctx;
    det_ctx.AddMetadata("authorization", "Bearer " + resp.token());

    const auto det_status = stub.TokenDetails(&det_ctx, det_req, &det_resp);
    if (!det_status.ok()) {
        spdlog::error("[Auth] TokenDetails() failed: code={} msg={}",
            det_status.error_code(), det_status.error_message());
        return std::unexpected(Error{ErrorCode::RpcError, det_status.error_message()});
    }
    for (const auto& id : det_resp.account_ids())
        account_ids_.push_back(id);

    if (account_ids_.empty()) {
        spdlog::error("[Auth] No account_ids in token");
        return std::unexpected(Error{ErrorCode::InvalidArgument, "no account_ids"});
    }
    spdlog::info("[Auth] accounts: {}", account_ids_.size());

    // 3. Запуск фонового стрима обновления JWT
    renewal_thread_ = std::thread(&TokenManager::run_renewal_stream,
                                  this, std::string(secret));
    return {};
}

// ── JWT accessors ─────────────────────────────────────────────────────────

std::string TokenManager::jwt() const noexcept {
    return *jwt_.load(std::memory_order_acquire);
}

std::shared_ptr<grpc::Channel> TokenManager::channel() const noexcept {
    return channel_;
}

const std::vector<std::string>& TokenManager::account_ids() const noexcept {
    return account_ids_;
}

std::string_view TokenManager::primary_account_id() const {
    if (account_ids_.empty())
        throw std::logic_error{"TokenManager::init() not called"};
    return account_ids_.front();
}

void TokenManager::shutdown() noexcept {
    stop_.store(true, std::memory_order_release);
    if (renewal_thread_.joinable()) renewal_thread_.join();
}

void TokenManager::store_jwt(std::string jwt) {
    jwt_.store(
        std::make_shared<const std::string>(std::move(jwt)),
        std::memory_order_release
    );
}

// ── SubscribeJwtRenewal стрим ─────────────────────────────────────────────

void TokenManager::run_renewal_stream(std::string secret) {
    spdlog::info("[Auth] JWT renewal stream started");

    while (!stop_.load(std::memory_order_acquire)) {
        auto stub = AuthStub(channel_);
        grpc::ClientContext ctx;

        ::grpc::tradeapi::v1::auth::SubscribeJwtRenewalRequest req;
        req.set_secret(secret);

        auto stream = stub.SubscribeJwtRenewal(&ctx, req);

        ::grpc::tradeapi::v1::auth::SubscribeJwtRenewalResponse resp;
        while (!stop_.load(std::memory_order_acquire) && stream->Read(&resp)) {
            if (!resp.token().empty()) {
                store_jwt(resp.token());
                spdlog::debug("[Auth] JWT renewed");
            }
        }

        if (stop_.load(std::memory_order_acquire)) break;

        const auto st = stream->Finish();
        spdlog::warn("[Auth] renewal stream ended: code={} msg={} — reconnect in 5s",
            st.error_code(), st.error_message());
        std::this_thread::sleep_for(std::chrono::seconds{5});
    }

    spdlog::info("[Auth] JWT renewal stream stopped");
}

} // namespace finam::auth
