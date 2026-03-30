#include <cstdlib>
#include <iostream>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "core/contract.hpp"

// ─────────────────────────────────────────────────────────────────────────
// connectivity_check:
//   1. Auth: secret-token → JWT
//   2. TokenDetails: проверяем account_ids
//   3. GetPortfolio: проверяем доступ к FORTS портфелю
//   4. Печатаем nearest_contract()
//   5. GetTradingInfo: проверяем .ГО для ближнего Si
//
// Exit code: 0 = OK, 1 = any step failed
// ─────────────────────────────────────────────────────────────────────────

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    // ── 1. Secret token ────────────────────────────────────────────────────────────
    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) {
        spdlog::critical("FINAM_SECRET_TOKEN not set");
        return 1;
    }

    // ── 2. Auth ──────────────────────────────────────────────────────────────────
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

    // ── 3. Account ──────────────────────────────────────────────────────────────
    const std::string account_id{token_mgr->primary_account_id()};
    if (account_id.empty()) {
        spdlog::critical("[2/4] FAIL: no account_id in TokenDetails");
        return 1;
    }
    spdlog::info("[2/4] OK: account_id={}", account_id);

    // ── 4. Portfolio (FORTS) ─────────────────────────────────────────────────────
    spdlog::info("[3/4] GetPortfolio (FORTS)...");
    if (auto r = token_mgr->check_portfolio_access(); !r) {
        spdlog::critical("[3/4] FAIL: {}", r.error().message);
        return 1;
    }
    spdlog::info("[3/4] OK: FORTS portfolio accessible");

    // ── 5. Nearest contract + TradingInfo ─────────────────────────────────────
    const auto sym = finam::core::nearest_contract("Si", 5);
    spdlog::info("[4/4] Nearest contract: {} — checking TradingInfo...",
        sym.to_string());
    if (auto r = token_mgr->check_trading_info(sym); !r) {
        spdlog::critical("[4/4] FAIL: {}", r.error().message);
        return 1;
    }
    spdlog::info("[4/4] OK");

    spdlog::info("✅  All checks passed. Bot is ready to start.");
    return 0;
}
