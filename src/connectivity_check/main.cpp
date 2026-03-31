#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "grpc/tradeapi/v1/accounts/accounts_service.grpc.pb.h"
#include "grpc/tradeapi/v1/assets/assets_service.grpc.pb.h"

// ──────────────────────────────────────────────────────────────────────────
// connectivity_check:
//   1. Auth → JWT
//   2. TokenDetails → account_id
//   3. GetAccount → equity
//   4. AllAssets(FORTS, only_active) → печатаем все активные фьючерсы FORTS
//      С фильтрацией по: mic=FORTS, type=futures
//      Выводим symbol (реальный формат ticker@MIC) для обновления contract.hpp
//   5. GetAssetParams для первого найденного Si-фьючерса
// ──────────────────────────────────────────────────────────────────────────

// Корневые тикеры которые нас интересуют (фильтруем по префиксу в name)
static const std::vector<std::string> kRoots = {
    "Si", "Ri", "Eu", "Br", "Ng", "Ed", "Gd", "Mx",
    "Lk", "Gz", "Sb", "Vb", "Rn", "Af", "Gm", "Ym",
    "Sv", "Pl", "Cu", "Al", "Nm", "Cr",
    // дополнительные на случай если префикс другой
    "Si", "RI", "GD", "MX", "BR", "NG", "GZ", "LK", "SB",
};

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
    spdlog::info("[3/5] equity={} forts_cash={}",
        resp.equity().value(),
        resp.portfolio_forts().available_cash().value());
    return {};
}

// Пагинация AllAssets с only_active=true.
// Фильтруем: mic==FORTS и type содержит "future" (кейс-инсенситивно).
// Возвращаем весь список активных фьючерсов FORTS.
static std::vector<::grpc::tradeapi::v1::assets::Asset>
list_forts_futures(const finam::auth::TokenManager& mgr)
{
    namespace AS = ::grpc::tradeapi::v1::assets;
    auto stub = AS::AssetsService::NewStub(mgr.channel());

    std::vector<AS::Asset> result;
    int64_t cursor = 0;
    int page = 0;

    while (true) {
        grpc::ClientContext ctx;
        ctx.AddMetadata("authorization", "Bearer " + mgr.jwt());

        AS::AllAssetsRequest req;
        req.set_cursor(cursor);
        req.set_only_active(true);

        AS::AllAssetsResponse resp;
        const auto st = stub->AllAssets(&ctx, req, &resp);
        if (!st.ok()) {
            spdlog::error("[4/5] AllAssets page={} failed: {}", page, st.error_message());
            break;
        }

        for (const auto& a : resp.assets()) {
            if (a.mic() != "FORTS") continue;
            // type может быть "futures", "future", "FUTURES" — проверяем case-insensitive
            std::string t = a.type();
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            if (t.find("fut") != std::string::npos)
                result.push_back(a);
        }

        spdlog::debug("[4/5] page={} assets={} forts_futures_so_far={} next_cursor={}",
            page, resp.assets_size(), result.size(), resp.next_cursor());

        if (resp.next_cursor() == 0 || resp.assets_size() == 0) break;
        cursor = resp.next_cursor();
        ++page;
    }
    return result;
}

static void check_asset_params(
    const finam::auth::TokenManager& mgr,
    const std::string& symbol)
{
    namespace AS = ::grpc::tradeapi::v1::assets;
    auto stub = AS::AssetsService::NewStub(mgr.channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("authorization", "Bearer " + mgr.jwt());
    AS::GetAssetParamsRequest req;
    req.set_symbol(symbol);
    req.set_account_id(std::string(mgr.primary_account_id()));
    AS::GetAssetParamsResponse resp;
    const auto st = stub->GetAssetParams(&ctx, req, &resp);
    if (!st.ok()) {
        spdlog::warn("[5/5] GetAssetParams({}) failed: {}", symbol, st.error_message());
        return;
    }
    spdlog::info("[5/5] {} long_go={} short_go={} tradable={}",
        symbol,
        resp.long_initial_margin().units(),
        resp.short_initial_margin().units(),
        resp.has_is_tradable() ? (resp.is_tradable().value() ? "yes" : "no") : "?");
}

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) { spdlog::critical("FINAM_SECRET_TOKEN not set"); return 1; }

    auto mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::TokenManagerConfig{.endpoint="api.finam.ru:443", .use_tls=true});

    spdlog::info("[1/5] Auth...");
    if (auto r = mgr->init(secret_env); !r) {
        spdlog::critical("[1/5] FAIL: {}", r.error().message); return 1;
    }
    spdlog::info("[1/5] OK  account_id={}", mgr->primary_account_id());
    spdlog::info("[2/5] OK");

    spdlog::info("[3/5] GetAccount...");
    if (auto r = check_account(*mgr); !r) {
        spdlog::critical("[3/5] FAIL: {}", r.error().message); return 1;
    }
    spdlog::info("[3/5] OK");

    spdlog::info("[4/5] AllAssets — ищем фьючерсы FORTS...");
    const auto futures = list_forts_futures(*mgr);
    spdlog::info("[4/5] Найдено {} активных фьючерсов FORTS", futures.size());

    if (futures.empty()) {
        spdlog::warn("[4/5] Ни одного фьючерса не найдено.");
        spdlog::warn("      Возможно: only_active фильтр не работает как ожидалось или type!=\"futures\"");
        spdlog::warn("      Смотри дополнительный вывод выше (spdlog::debug)");
        return 1;
    }

    spdlog::info("");
    spdlog::info("══ ВСЕ ФЬЮЧЕРСЫ FORTS (symbol | ticker | name) ══");
    std::string first_si;
    for (const auto& a : futures) {
        spdlog::info("  symbol={:<30s} ticker={:<12s} name={}",
            a.symbol(), a.ticker(), a.name());
        // Первый Si-похожий — для шага 5
        if (first_si.empty()) {
            const std::string& n = a.name();
            if (n.find("Si") != std::string::npos ||
                n.find("доллар") != std::string::npos ||
                a.ticker().rfind("Si", 0) == 0)
            {
                first_si = a.symbol();
            }
        }
    }

    if (!first_si.empty()) {
        spdlog::info("");
        spdlog::info("[5/5] GetAssetParams для {} (GO)...", first_si);
        check_asset_params(*mgr, first_si);
    }

    spdlog::info("");
    spdlog::info("✔  Скопируй symbol из таблицы выше — это реальные значения для contract.hpp");
    return 0;
}
