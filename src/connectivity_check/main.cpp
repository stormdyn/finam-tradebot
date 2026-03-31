#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "grpc/tradeapi/v1/accounts/accounts_service.grpc.pb.h"
#include "grpc/tradeapi/v1/assets/assets_service.grpc.pb.h"

static finam::Result<void> check_account(const finam::auth::TokenManager& mgr) {
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
        resp.equity().value(), resp.portfolio_forts().available_cash().value());
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

    // ── Полный обход AllAssets через пагинацию ──────────────────────
    spdlog::info("[4/4] AllAssets full scan — ищем FORTS...");

    namespace AS = ::grpc::tradeapi::v1::assets;
    auto stub = AS::AssetsService::NewStub(mgr->channel());

    int64_t cursor    = 0;
    int     page      = 0;
    int     total     = 0;
    std::set<std::string> forts_types;  // уникальные type у FORTS-инструментов

    std::vector<AS::Asset> forts_assets;

    while (true) {
        grpc::ClientContext ctx;
        ctx.AddMetadata("authorization", "Bearer " + mgr->jwt());
        AS::AllAssetsRequest req;
        req.set_cursor(cursor);
        req.set_only_active(true);
        AS::AllAssetsResponse resp;

        const auto st = stub->AllAssets(&ctx, req, &resp);
        if (!st.ok()) {
            spdlog::error("[4/4] page={} FAIL: {}", page, st.error_message());
            break;
        }

        total += resp.assets_size();
        for (const auto& a : resp.assets()) {
            if (a.mic() == "FORTS") {
                forts_types.insert(a.type());
                forts_assets.push_back(a);
            }
        }

        spdlog::info("[4/4] page={} got={} total={} forts_so_far={} next_cursor={}",
            page, resp.assets_size(), total, forts_assets.size(), resp.next_cursor());

        if (resp.next_cursor() == 0 || resp.assets_size() == 0) break;
        cursor = resp.next_cursor();
        ++page;
    }

    spdlog::info("");
    spdlog::info("══ FORTS: {} инструментов, уникальных type: {} ══",
        forts_assets.size(), forts_types.size());
    for (const auto& t : forts_types)
        spdlog::info("  type = '{}'", t);

    spdlog::info("");
    spdlog::info("══ ВСЕ FORTS инструменты (symbol | ticker | type | name) ══");
    for (const auto& a : forts_assets) {
        spdlog::info("  symbol={:<30s} ticker={:<12s} type={:<20s} name={}",
            a.symbol(), a.ticker(), a.type(), a.name());
    }

    return forts_assets.empty() ? 1 : 0;
}
