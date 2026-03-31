#include <cstdlib>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "grpc/tradeapi/v1/accounts/accounts_service.grpc.pb.h"
#include "grpc/tradeapi/v1/assets/assets_service.grpc.pb.h"

// ──────────────────────────────────────────────────────────────────────────
// connectivity_check:
//   Exchanges() показал: срочный рынок MOEX = mic=RTSX
//   ("MOSCOW EXCHANGE - DERIVATIVES MARKET")
//   FORTS — не MIC в этом API, правильный MIC: RTSX
//
//   Текущий квартал июнь 2026 — буква M
//   Форматы тикера FORTS: {ROOT}{LETTER}{YY}, напр.: SiM6, RIM6, GDM6
// ──────────────────────────────────────────────────────────────────────────

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
    spdlog::info("equity={}", resp.equity().value());
    return {};
}

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) { spdlog::critical("FINAM_SECRET_TOKEN not set"); return 1; }

    auto mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::TokenManagerConfig{.endpoint="api.finam.ru:443", .use_tls=true});

    if (auto r = mgr->init(secret_env); !r) {
        spdlog::critical("Auth FAIL: {}", r.error().message); return 1;
    }
    spdlog::info("Auth OK  account_id={}", mgr->primary_account_id());
    if (auto r = check_account(*mgr); !r) {
        spdlog::critical("GetAccount FAIL: {}", r.error().message); return 1;
    }

    namespace AS = ::grpc::tradeapi::v1::assets;
    auto stub = AS::AssetsService::NewStub(mgr->channel());

    // ── Проба GetAsset: mic=RTSX, разные форматы тикера ──────────────────
    // Июнь 2026 = квартал M, год 26
    const std::vector<std::string> candidates = {
        // RTSX + разные форматы
        "SiM6@RTSX",    "SIM6@RTSX",
        "SiM26@RTSX",   "SIM26@RTSX",
        "Si-6.26@RTSX",
        // RUSX (ещё один кандидат)
        "SiM6@RUSX",    "SIM6@RUSX",
        // Также пробуем RTS и GOLD с RTSX
        "RIM6@RTSX",    "RITM6@RTSX",  "RiM6@RTSX",
        "GDM6@RTSX",    "GOLDM6@RTSX",
        "MXM6@RTSX",    "BRM6@RTSX",
        // ещё попробуем через MISX (вдруг срочный рынок под одним MIC)
        "SiM6@MISX",
    };

    spdlog::info("");
    spdlog::info("══ GetAsset проба (mic=RTSX): ══");
    bool any_ok = false;
    for (const auto& sym : candidates) {
        grpc::ClientContext ctx;
        ctx.AddMetadata("authorization", "Bearer " + mgr->jwt());
        AS::GetAssetRequest req;
        req.set_symbol(sym);
        req.set_account_id(std::string(mgr->primary_account_id()));
        AS::GetAssetResponse resp;
        const auto st = stub->GetAsset(&ctx, req, &resp);
        if (st.ok()) {
            spdlog::info("  ✔ symbol={:<25s} board={} ticker={} mic={} type={} lot={} decimals={} expiry={}-{}-{}",
                sym, resp.board(), resp.ticker(), resp.mic(), resp.type(),
                resp.lot_size().value(), resp.decimals(),
                resp.expiration_date().year(),
                resp.expiration_date().month(),
                resp.expiration_date().day());
            any_ok = true;
        } else {
            spdlog::info("  ✗ {:<25s} => {}", sym, st.error_message());
        }
    }

    if (!any_ok) {
        spdlog::warn("");
        spdlog::warn("Ни один кандидат не сработал.");
        spdlog::warn("Попробуй AllAssets с mic=RTSX через пагинацию — добавь фильтр в коде");
    }

    return any_ok ? 0 : 1;
}
