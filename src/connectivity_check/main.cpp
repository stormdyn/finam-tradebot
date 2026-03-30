#include <cstdlib>
#include <iostream>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "core/contract.hpp"

// gRPC stubs for live API checks
#include "grpc/tradeapi/v1/accounts/accounts_service.grpc.pb.h"
#include "grpc/tradeapi/v1/assets/assets_service.grpc.pb.h"

// ─────────────────────────────────────────────────────────────────────────
// connectivity_check:
//   1. Auth: secret-token -> JWT
//   2. TokenDetails: account_ids
//   3. GetAccount: FORTS portfolio (equity, available_cash)
//   4. GetAssetParams: initial margin (GO) for nearest Si
//
// Exit code: 0 = OK, 1 = any step failed
// ─────────────────────────────────────────────────────────────────────────

// Real API: AccountsService.GetAccount (not GetPortfolio — that method doesn't exist)
static finam::Result<void> check_account(
    const finam::auth::TokenManager& mgr)
{
    namespace proto_acc = ::grpc::tradeapi::v1::accounts;

    auto stub = proto_acc::AccountsService::NewStub(mgr.channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("authorization", "Bearer " + mgr.jwt());

    proto_acc::GetAccountRequest req;
    req.set_account_id(std::string(mgr.primary_account_id()));

    proto_acc::GetAccountResponse resp;
    const auto status = stub->GetAccount(&ctx, req, &resp);
    if (!status.ok()) {
        return std::unexpected(finam::Error{
            finam::ErrorCode::RpcError,
            status.error_message()
        });
    }
    // equity = portfolio value; portfolio_forts.available_cash = free margin
    spdlog::info("[3/4] account type={} equity={} available_cash={}",
        resp.type(),
        resp.equity().value(),
        resp.portfolio_forts().available_cash().value());
    return {};
}

// Real API: AssetsService.GetAssetParams (not GetTradingInfo — that method doesn't exist)
// Returns long_initial_margin / short_initial_margin (GO for FORTS) as google.type.Money
static finam::Result<void> check_asset_params(
    const finam::auth::TokenManager& mgr,
    const finam::Symbol& sym)
{
    namespace proto_assets = ::grpc::tradeapi::v1::assets;

    auto stub = proto_assets::AssetsService::NewStub(mgr.channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("authorization", "Bearer " + mgr.jwt());

    proto_assets::GetAssetParamsRequest req;
    req.set_symbol(sym.to_string());
    req.set_account_id(std::string(mgr.primary_account_id()));

    proto_assets::GetAssetParamsResponse resp;
    const auto status = stub->GetAssetParams(&ctx, req, &resp);
    if (!status.ok()) {
        return std::unexpected(finam::Error{
            finam::ErrorCode::RpcError,
            status.error_message()
        });
    }
    spdlog::info("[4/4] long_initial_margin={} short_initial_margin={}",
        resp.long_initial_margin().units(),
        resp.short_initial_margin().units());
    return {};
}

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) {
        spdlog::critical("FINAM_SECRET_TOKEN not set");
        return 1;
    }

    auto token_mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::TokenManagerConfig{
            .endpoint = "api.finam.ru:443",
            .use_tls  = true,
        }
    );

    spdlog::info("[1/4] Auth: obtaining JWT...");
    if (auto r = token_mgr->init(secret_env); !r) {
        spdlog::critical("[1/4] FAIL: {}", r.error().message);
        return 1;
    }
    spdlog::info("[1/4] OK: JWT obtained");

    const std::string account_id{token_mgr->primary_account_id()};
    if (account_id.empty()) {
        spdlog::critical("[2/4] FAIL: no account_id in TokenDetails");
        return 1;
    }
    spdlog::info("[2/4] OK: account_id={}", account_id);

    spdlog::info("[3/4] GetAccount (FORTS portfolio)...");
    if (auto r = check_account(*token_mgr); !r) {
        spdlog::critical("[3/4] FAIL: {}", r.error().message);
        return 1;
    }
    spdlog::info("[3/4] OK");

    const auto sym = finam::core::nearest_contract("Si", 5);
    spdlog::info("[4/4] Nearest contract: {} -- checking AssetParams (GO)...",
        sym.to_string());
    if (auto r = check_asset_params(*token_mgr, sym); !r) {
        spdlog::critical("[4/4] FAIL: {}", r.error().message);
        return 1;
    }
    spdlog::info("[4/4] OK");

    spdlog::info("All checks passed. Bot is ready to start.");
    return 0;
}
