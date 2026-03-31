#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "grpc/tradeapi/v1/accounts/accounts_service.grpc.pb.h"
#include "grpc/tradeapi/v1/assets/assets_service.grpc.pb.h"

// ──────────────────────────────────────────────────────────────────────────
// connectivity_check ─ цель: узнать реальные symbol фьючерсов FORTS.
//
// Алгоритм:
//   1. AllAssets(only_active=true, cursor=0) — без фильтра по type
//   2. Печатаем ВСЕ инструменты с mic==FORTS (первая страница)
//   3. Видим реальные значения type, symbol и name
// ──────────────────────────────────────────────────────────────────────────

static finam::Result<void> check_account(
    const finam::auth::TokenManager& mgr)
{
    namespace A = ::grpc::tradeapi::v1::accounts;
    auto stub = A::AccountsService::NewStub(mgr.channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("authorization", "Bearer " + mgr.jwt());
    A::GetAccountRequest req;
    req.set_account_id(std::string(mgr.primary_account_id()));
    A::GetAccountResponse resp;
    if (const auto st = stub->GetAccount(&ctx, req, &resp); !st.ok())
        return std::unexpected(finam::Error{finam::ErrorCode::RpcError, st.error_message()});
    spdlog::info("[3/4] equity={} forts_cash={}",
        resp.equity().value(),
        resp.portfolio_forts().available_cash().value());
    return {};
}

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) { spdlog::critical("FINAM_SECRET_TOKEN not set"); return 1; }

    auto mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::TokenManagerConfig{.endpoint="api.finam.ru:443", .use_tls=true});

    spdlog::info("[1/4] Auth...");
    if (auto r = mgr->init(secret_env); !r) {
        spdlog::critical("[1/4] FAIL: {}", r.error().message); return 1;
    }
    spdlog::info("[1/4] OK  account_id={}", mgr->primary_account_id());

    spdlog::info("[2/4] OK");

    spdlog::info("[3/4] GetAccount...");
    if (auto r = check_account(*mgr); !r) {
        spdlog::critical("[3/4] FAIL: {}", r.error().message); return 1;
    }
    spdlog::info("[3/4] OK");

    // ── AllAssets: первая страница, без фильтров ──────────────────────────
    // Цель: увидеть реальные значения type и формат symbol для FORTS
    spdlog::info("[4/4] AllAssets(only_active=true, page 0) — все FORTS инструменты:");

    namespace AS = ::grpc::tradeapi::v1::assets;
    auto stub = AS::AssetsService::NewStub(mgr->channel());

    grpc::ClientContext ctx;
    ctx.AddMetadata("authorization", "Bearer " + mgr->jwt());

    AS::AllAssetsRequest req;
    req.set_cursor(0);
    req.set_only_active(true);

    AS::AllAssetsResponse resp;
    const auto st = stub->AllAssets(&ctx, req, &resp);
    if (!st.ok()) {
        spdlog::critical("[4/4] AllAssets FAIL: {}", st.error_message());
        return 1;
    }

    spdlog::info("[4/4] Страница 0: {} инструментов, next_cursor={}",
        resp.assets_size(), resp.next_cursor());

    // Печатаем все FORTS-инструменты — видим реальные type
    int forts_count = 0;
    for (const auto& a : resp.assets()) {
        if (a.mic() != "FORTS") continue;
        spdlog::info("  mic={} type={:<20s} symbol={:<30s} ticker={:<12s} name={}",
            a.mic(), a.type(), a.symbol(), a.ticker(), a.name());
        ++forts_count;
    }

    if (forts_count == 0) {
        // Если FORTS-инструментов нет вообще — печатаем первые 20 любых
        spdlog::warn("  Ни одного FORTS. Первые 20 инструментов (любых):");
        int n = 0;
        for (const auto& a : resp.assets()) {
            spdlog::warn("  mic={} type={:<20s} symbol={:<30s} name={}",
                a.mic(), a.type(), a.symbol(), a.name());
            if (++n >= 20) break;
        }
    }

    spdlog::info("");
    spdlog::info("✔  Скопируй type фьючерсов из вывода выше — обновим фильтр в коде");
    return 0;
}
