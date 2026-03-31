#include <cstdlib>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>

#include "auth/token_manager.hpp"
#include "core/contract.hpp"

#include "grpc/tradeapi/v1/accounts/accounts_service.grpc.pb.h"
#include "grpc/tradeapi/v1/assets/assets_service.grpc.pb.h"

// ──────────────────────────────────────────────────────────────────────────
// connectivity_check ─ 5 шагов:
//   1. Auth → JWT
//   2. TokenDetails → account_id
//   3. GetAccount → FORTS портфель
//   4. Пробируем реальные symbol для 20 фьючерсов FORTS через GetAssetParams
//   5. Печатаем таблицу работающих символов + GO
// ──────────────────────────────────────────────────────────────────────────

// Кандидаты формата symbol для каждого тикера.
// Текущий квартал: июнь 2026.
// Форматы для пробы: "{TICKER}-{M}.{YY}@FORTS" и "{TICKER}{LETTER}{YY}@FORTS"
// LETTER: H=март, M=июнь, U=сентябрь, Z=декабрь
static const std::vector<std::pair<std::string, std::vector<std::string>>> kProbes = {
    // {ticker, {candidates...}}
    {"Si",      {"Si-6.26@FORTS",   "SiM6@FORTS",    "SIM26@FORTS",  "Si6.26@FORTS"}},
    {"RTS",     {"RTS-6.26@FORTS",  "RiM6@FORTS",    "RIM26@FORTS",  "RTS6.26@FORTS"}},
    {"GOLD",    {"GOLD-6.26@FORTS", "GDM6@FORTS",    "GDM26@FORTS",  "GOLD6.26@FORTS"}},
    {"MIX",     {"MIX-6.26@FORTS",  "MXM6@FORTS",    "MXM26@FORTS",  "MIX6.26@FORTS"}},
    {"BR",      {"BR-6.26@FORTS",   "BRM6@FORTS",    "BRM26@FORTS",  "BR6.26@FORTS"}},
    {"NG",      {"NG-6.26@FORTS",   "NGM6@FORTS",    "NGM26@FORTS",  "NG6.26@FORTS"}},
    {"ED",      {"ED-6.26@FORTS",   "EDM6@FORTS",    "EDM26@FORTS",  "ED6.26@FORTS"}},
    {"CNYRUB",  {"CNYRUB-6.26@FORTS","CRM6@FORTS",   "CRM26@FORTS"}},
    {"GLDRUB",  {"GLDRUB-6.26@FORTS","GZM6@FORTS",   "GZM26@FORTS"}},
    {"EUR",     {"EUR-6.26@FORTS",   "EUM6@FORTS",   "EUM26@FORTS"}},
    {"SILV",    {"SILV-6.26@FORTS",  "SVM6@FORTS",   "SVM26@FORTS",  "SILV6.26@FORTS"}},
    {"PLAT",    {"PLAT-6.26@FORTS",  "PLM6@FORTS",   "PLM26@FORTS"}},
    {"CU",      {"CU-6.26@FORTS",    "CUM6@FORTS",   "CUM26@FORTS"}},
    {"AL",      {"AL-6.26@FORTS",    "ALM6@FORTS",   "ALM26@FORTS"}},
    {"Ni",      {"Ni-6.26@FORTS",    "NIM6@FORTS",   "NIM26@FORTS"}},
    {"GAZR",    {"GAZR-6.26@FORTS",  "GZM6@FORTS",   "GZM26@FORTS"}},
    {"LKOH",    {"LKOH-6.26@FORTS",  "LKM6@FORTS",   "LKM26@FORTS"}},
    {"SBER",    {"SBER-6.26@FORTS",  "SBM6@FORTS",   "SBM26@FORTS"}},
    {"VTBR",    {"VTBR-6.26@FORTS",  "VBM6@FORTS",   "VBM26@FORTS"}},
    {"ROSN",    {"ROSN-6.26@FORTS",  "RNM6@FORTS",   "RNM26@FORTS"}},
};

struct ProbeResult {
    std::string ticker;
    std::string symbol;   // рабочий symbol
    int64_t     long_go{};
    int64_t     short_go{};
};

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
    const auto st = stub->GetAccount(&ctx, req, &resp);
    if (!st.ok())
        return std::unexpected(finam::Error{finam::ErrorCode::RpcError, st.error_message()});
    spdlog::info("[3/5] equity={} available_cash={}",
        resp.equity().value(),
        resp.portfolio_forts().available_cash().value());
    return {};
}

// Пробируем кандидаты symbol через GetAssetParams — первый успешный запрос даёт GO
static std::optional<ProbeResult> probe_ticker(
    const finam::auth::TokenManager& mgr,
    const std::string& ticker,
    const std::vector<std::string>& candidates)
{
    namespace pa = ::grpc::tradeapi::v1::assets;
    auto stub = pa::AssetsService::NewStub(mgr.channel());

    for (const auto& sym : candidates) {
        grpc::ClientContext ctx;
        ctx.AddMetadata("authorization", "Bearer " + mgr.jwt());

        pa::GetAssetParamsRequest req;  // реальное имя метода в proto
        req.set_symbol(sym);
        req.set_account_id(std::string(mgr.primary_account_id()));

        pa::GetAssetParamsResponse resp;
        const auto st = stub->GetAssetParams(&ctx, req, &resp);
        if (st.ok()) {
            return ProbeResult{
                .ticker   = ticker,
                .symbol   = sym,
                .long_go  = resp.long_initial_margin().units(),
                .short_go = resp.short_initial_margin().units(),
            };
        }
        spdlog::debug("[4/5]   {} ✗ ({})", sym, st.error_message());
    }
    return std::nullopt;
}

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    const char* secret_env = std::getenv("FINAM_SECRET_TOKEN");
    if (!secret_env) {
        spdlog::critical("FINAM_SECRET_TOKEN not set");
        return 1;
    }

    auto mgr = std::make_shared<finam::auth::TokenManager>(
        finam::auth::TokenManagerConfig{.endpoint="api.finam.ru:443", .use_tls=true});

    spdlog::info("[1/5] Auth...");
    if (auto r = mgr->init(secret_env); !r) {
        spdlog::critical("[1/5] FAIL: {}", r.error().message); return 1;
    }
    spdlog::info("[1/5] OK");
    spdlog::info("[2/5] account_id={}", mgr->primary_account_id());

    spdlog::info("[3/5] GetAccount...");
    if (auto r = check_account(*mgr); !r) {
        spdlog::critical("[3/5] FAIL: {}", r.error().message); return 1;
    }
    spdlog::info("[3/5] OK");

    spdlog::info("[4/5] Пробируем {} тикеров FORTS...", kProbes.size());
    std::vector<ProbeResult> found;
    std::vector<std::string> not_found;

    for (const auto& [ticker, candidates] : kProbes) {
        if (auto r = probe_ticker(*mgr, ticker, candidates)) {
            found.push_back(*r);
            spdlog::info("[4/5] ✔ {:8s} => symbol={:<30s} long_go={} short_go={}",
                ticker, r->symbol, r->long_go, r->short_go);
        } else {
            not_found.push_back(ticker);
            spdlog::warn("[4/5] ✗ {:8s} => ни один кандидат не сработал", ticker);
        }
    }

    spdlog::info("");
    spdlog::info("[5/5] Результат: {}/{} тикеров найдено",
        found.size(), kProbes.size());
    if (!not_found.empty()) {
        std::string s;
        for (auto& t : not_found) s += t + " ";
        spdlog::warn("[5/5] Не найдено: {}", s);
    }

    spdlog::info("");
    spdlog::info("══ Скопируй реальные symbol для обновления contract.hpp и main.cpp ══");
    for (const auto& r : found)
        spdlog::info("  {} => {}", r.ticker, r.symbol);

    return found.empty() ? 1 : 0;
}
