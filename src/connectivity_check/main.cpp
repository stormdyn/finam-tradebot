#include <cstdlib>
#include <iostream>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "core/contract.hpp"

#include "grpc/tradeapi/v1/accounts/accounts_service.grpc.pb.h"
#include "grpc/tradeapi/v1/assets/assets_service.grpc.pb.h"

// ──────────────────────────────────────────────────────────────────────────
// connectivity_check:
//   1. Auth: secret-token -> JWT
//   2. TokenDetails: account_ids
//   3. GetAccount: FORTS portfolio
//   4. GetAssets: печатаем реальные символы Si/RTS/GOLD/MIX с площадки FORTS
//   5. GetAssetParams: GO для первого найденного Si-символа
// ──────────────────────────────────────────────────────────────────────────

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
    if (!status.ok())
        return std::unexpected(finam::Error{finam::ErrorCode::RpcError, status.error_message()});

    spdlog::info("[3/5] account type={} equity={} available_cash={}",
        resp.type(),
        resp.equity().value(),
        resp.portfolio_forts().available_cash().value());
    return {};
}

// Печатаем все активы на FORTS чьи short_name содержит подстроку (Si/RTS/GOLD/MIX).
// Цель: узнать реальный формат symbol который API принимает в GetAssetParams.
static std::string find_si_symbol(
    const finam::auth::TokenManager& mgr)
{
    namespace proto_assets = ::grpc::tradeapi::v1::assets;

    auto stub = proto_assets::AssetsService::NewStub(mgr.channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("authorization", "Bearer " + mgr.jwt());

    proto_assets::GetAssetsRequest req;
    // Фильтруем по mic=FORTS если API поддерживает это поле
    req.set_mic("FORTS");

    proto_assets::GetAssetsResponse resp;
    const auto status = stub->GetAssets(&ctx, req, &resp);
    if (!status.ok()) {
        spdlog::warn("[4/5] GetAssets(mic=FORTS) failed: {} — retrying without filter",
            status.error_message());
        // Попробуем без фильтра
        grpc::ClientContext ctx2;
        ctx2.AddMetadata("authorization", "Bearer " + mgr.jwt());
        proto_assets::GetAssetsRequest req2;
        const auto st2 = stub->GetAssets(&ctx2, req2, &resp);
        if (!st2.ok()) {
            spdlog::error("[4/5] GetAssets failed: {}", st2.error_message());
            return {};
        }
    }

    spdlog::info("[4/5] GetAssets returned {} assets total", resp.assets_size());

    // Ищем фьючерсы Si/RTS/GOLD/MIX и печатаем их symbol
    static const std::vector<std::string> kTickers = {"Si", "RTS", "GOLD", "MIX"};
    std::string first_si;

    for (const auto& a : resp.assets()) {
        const std::string& sym  = a.symbol();
        const std::string& name = a.short_name();
        const std::string& mic  = a.mic();

        for (const auto& tk : kTickers) {
            // short_name начинается с тикера
            if (name.rfind(tk, 0) == 0 || sym.rfind(tk, 0) == 0) {
                spdlog::info("[4/5]   {} | mic={} | short_name={} | symbol={}",
                    tk, mic, name, sym);
                if (tk == "Si" && first_si.empty())
                    first_si = sym;
            }
        }
    }

    return first_si;
}

static finam::Result<void> check_asset_params(
    const finam::auth::TokenManager& mgr,
    const std::string& symbol)
{
    namespace proto_assets = ::grpc::tradeapi::v1::assets;

    auto stub = proto_assets::AssetsService::NewStub(mgr.channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("authorization", "Bearer " + mgr.jwt());

    proto_assets::GetAssetParamsRequest req;
    req.set_symbol(symbol);
    req.set_account_id(std::string(mgr.primary_account_id()));

    proto_assets::GetAssetParamsResponse resp;
    const auto status = stub->GetAssetParams(&ctx, req, &resp);
    if (!status.ok())
        return std::unexpected(finam::Error{finam::ErrorCode::RpcError, status.error_message()});

    spdlog::info("[5/5] symbol={} long_go={} short_go={}",
        symbol,
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

    spdlog::info("[1/5] Auth: obtaining JWT...");
    if (auto r = token_mgr->init(secret_env); !r) {
        spdlog::critical("[1/5] FAIL: {}", r.error().message);
        return 1;
    }
    spdlog::info("[1/5] OK: JWT obtained, account_id={}",
        token_mgr->primary_account_id());

    const std::string account_id{token_mgr->primary_account_id()};
    if (account_id.empty()) {
        spdlog::critical("[2/5] FAIL: no account_id");
        return 1;
    }
    spdlog::info("[2/5] OK: account_id={}", account_id);

    spdlog::info("[3/5] GetAccount (FORTS portfolio)...");
    if (auto r = check_account(*token_mgr); !r) {
        spdlog::critical("[3/5] FAIL: {}", r.error().message);
        return 1;
    }
    spdlog::info("[3/5] OK");

    spdlog::info("[4/5] GetAssets: ищем Si/RTS/GOLD/MIX на FORTS...");
    const std::string si_sym = find_si_symbol(*token_mgr);
    if (si_sym.empty()) {
        spdlog::warn("[4/5] Si не найден — смотри лог выше для реальных названий");
        return 1;
    }
    spdlog::info("[4/5] OK: first Si symbol = {}", si_sym);

    spdlog::info("[5/5] GetAssetParams (GO) for {}...", si_sym);
    if (auto r = check_asset_params(*token_mgr, si_sym); !r) {
        spdlog::critical("[5/5] FAIL: {}", r.error().message);
        return 1;
    }
    spdlog::info("[5/5] OK");

    spdlog::info("");
    spdlog::info("✔  All checks passed.");
    spdlog::info("⚠  Скопируй symbol Si из лога выше (строка '[4/5]   Si') и обнови contract.hpp.");
    return 0;
}
