#include "token_manager.hpp"
#include <stdexcept>
#include <spdlog/spdlog.h>

// TODO: подключить после первой успешной компиляции proto
// #include "grpc/tradeapi/v1/auth/auth_service.grpc.pb.h"

namespace finam::auth {

TokenManager::TokenManager(Config cfg)
    : cfg_(std::move(cfg))
    , jwt_(std::make_shared<const std::string>())
{}

TokenManager::~TokenManager() { shutdown(); }

Result<void> TokenManager::init() {
    spdlog::info("[Auth] stub init — proto not compiled yet");
    store_jwt("stub-jwt");
    account_ids_.push_back("stub-account");
    return {};
}

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

Result<void> TokenManager::fetch_account_ids() { return {}; }
void TokenManager::run_renewal_stream() {}

} // namespace finam::auth