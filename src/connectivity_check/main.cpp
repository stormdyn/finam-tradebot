#include <cstdlib>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "grpc/tradeapi/v1/accounts/accounts_service.grpc.pb.h"
#include "grpc/tradeapi/v1/assets/assets_service.grpc.pb.h"

// ──────────────────────────────────────────────────────────────────────────
// connectivity_check:
//   1. Auth
//   2. GetAccount (equity)
//   3. Exchanges() — все MIC-коды доступных бирж
//   4. GetAsset для набора кандидатов Si-фьючерса (разные MIC и тикеры)
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
    spdlog::info("[2/4] equity={}", resp.equity().value());
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

    if (auto r = check_account(*mgr); !r) {
        spdlog::critical("[2/4] FAIL: {}", r.error().message); return 1;
    }

    namespace AS = ::grpc::tradeapi::v1::assets;
    auto stub = AS::AssetsService::NewStub(mgr->channel());

    // ── 3. Exchanges — все доступные MIC коды ────────────────────────────
    {
        grpc::ClientContext ctx;
        ctx.AddMetadata("authorization", "Bearer " + mgr->jwt());
        AS::ExchangesRequest req;
        AS::ExchangesResponse resp;
        const auto st = stub->Exchanges(&ctx, req, &resp);
        if (!st.ok()) {
            spdlog::error("[3/4] Exchanges FAIL: {}", st.error_message());
        } else {
            spdlog::info("[3/4] Exchanges: {} бирж:", resp.exchanges_size());
            for (const auto& e : resp.exchanges())
                spdlog::info("  mic={:<20s}  name={}", e.mic(), e.name());
        }
    }

    // ── 4. Проба GetAsset для Si: разные MIC + тикеры ────────────────
    // Буквы кварталов: H=март, M=июнь, U=сентябрь, Z=декабрь
    // Текущий квартал июнь 2026
    const std::vector<std::string> si_candidates = {
        // формат ticker@MIC с разными MIC-кодами
        "SiM6@FORTS",  "SIM6@FORTS",  "SiM26@FORTS", "SIM26@FORTS",
        "Si-6.26@FORTS",
        // возможные альтернативные MIC
        "SiM6@RFUD",   "SiM6@SPBFUT", "SiM6@SPBX",
        "SiM6@MOEX",   "SiM6@XMOS",
        // без года
        "SiM@FORTS",   "Si-M@FORTS",
        // полная дата
        "Si-6.2026@FORTS",
    };

    spdlog::info("[4/4] GetAsset проба для Si:");
    for (const auto& sym : si_candidates) {
        grpc::ClientContext ctx;
        ctx.AddMetadata("authorization", "Bearer " + mgr->jwt());
        AS::GetAssetRequest req;
        req.set_symbol(sym);
        req.set_account_id(std::string(mgr->primary_account_id()));
        AS::GetAssetResponse resp;
        const auto st = stub->GetAsset(&ctx, req, &resp);
        if (st.ok()) {
            spdlog::info("  ✔ symbol={} => board={} ticker={} mic={} type={} decimals={} expiry={}-{}-{}",
                sym, resp.board(), resp.ticker(), resp.mic(), resp.type(),
                resp.decimals(),
                resp.expiration_date().year(),
                resp.expiration_date().month(),
                resp.expiration_date().day());
        } else {
            spdlog::info("  ✗ {} => {}", sym, st.error_message());
        }
    }

    return 0;
}
