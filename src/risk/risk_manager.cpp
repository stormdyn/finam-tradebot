#include "risk_manager.hpp"
#include "core/contract.hpp"
#include <thread>
#include <shared_mutex>
#include <unordered_map>
#include <charconv>
#include <core/grpc_fmt.hpp>
#include <spdlog/spdlog.h>

#include "grpc/tradeapi/v1/accounts/accounts_service.grpc.pb.h"
#include "grpc/tradeapi/v1/assets/assets_service.grpc.pb.h"

namespace proto_acc    = ::grpc::tradeapi::v1::accounts;
namespace proto_assets = ::grpc::tradeapi::v1::assets;

namespace finam::risk {

namespace {

double decimal_to_double(const google::type::Decimal& d) noexcept {
    try { return std::stod(d.value()); } catch (...) { return 0.0; }
}

} // namespace

// ── ctor / dtor ────────────────────────────────────────────────────────────────────────

RiskManager::RiskManager(
    RiskConfig                          cfg,
    std::shared_ptr<auth::TokenManager> token_mgr)
    : cfg_(cfg)
    , token_mgr_(std::move(token_mgr))
{}

RiskManager::~RiskManager() { shutdown(); }

// ── start / shutdown ─────────────────────────────────────────────────────────────────

void RiskManager::start(std::chrono::seconds poll_interval) {
    if (auto r = refresh_account(); !r)
        spdlog::warn("[Risk] initial account refresh failed: {}", r.error().message);
    poll_thread_ = std::thread(&RiskManager::poll_loop, this, poll_interval);
    spdlog::info("[Risk] started, poll={}s", poll_interval.count());
}

void RiskManager::shutdown() noexcept {
    stop_.store(true, std::memory_order_release);
    if (poll_thread_.joinable()) poll_thread_.join();
}

void RiskManager::poll_loop(std::chrono::seconds interval) {
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(interval);
        if (stop_.load(std::memory_order_acquire)) break;
        if (auto r = refresh_account(); !r)
            spdlog::warn("[Risk] account refresh failed: {}", r.error().message);
    }
}

// ── refresh_account ────────────────────────────────────────────────────────────────

Result<void> RiskManager::refresh_account() {
    auto stub = proto_acc::AccountsService::NewStub(token_mgr_->channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("authorization", "Bearer " + token_mgr_->jwt());

    proto_acc::GetPortfolioRequest req;
    req.set_account_id(std::string(token_mgr_->primary_account_id()));

    proto_acc::GetPortfolioResponse resp;
    const auto status = stub->GetPortfolio(&ctx, req, &resp);
    if (!status.ok()) {
        spdlog::error("[Risk] GetPortfolio failed: code={} msg={}",
            status.error_code(), status.error_message());
        return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    }

    const auto& forts = resp.portfolio_forts();
    AccountState s;
    s.liquid_value   = decimal_to_double(forts.liquid_value());
    s.used_margin    = decimal_to_double(forts.used_margin());
    s.free_margin    = decimal_to_double(forts.free_margin());
    s.open_positions = static_cast<double>(forts.positions_size());
    s.daily_pnl      = decimal_to_double(forts.daily_pnl());
    s.updated_at     = std::chrono::system_clock::now();

    {
        std::unique_lock lock(mu_);
        state_ = s;
        if (s.liquid_value > peak_liquid_)
            peak_liquid_ = s.liquid_value;
    }
    spdlog::debug("[Risk] account: liquid={:.0f} free_margin={:.0f} "
                  "daily_pnl={:.0f} positions={:.0f}",
        s.liquid_value, s.free_margin, s.daily_pnl, s.open_positions);
    return {};
}

// ── on_fill ───────────────────────────────────────────────────────────────────────────
//
// Подсчёт PnL: если ордер закрывает открытую позицию — считаем pnl = delta * qty.
// Если открывает новую — запоминаем entry_price для будущего подсчёта.
// Открытие: Sell закрывает long, Buy закрывает short.

void RiskManager::on_fill(const OrderUpdate& upd) noexcept {
    if (upd.qty_filled <= 0) return;

    std::unique_lock lock(mu_);
    const int32_t tid  = upd.transaction_id;
    const bool is_buy  = (upd.side == OrderSide::Buy);

    // Поищем открытую позицию с противоположной стороны
    auto it = std::find_if(
        open_pos_.begin(), open_pos_.end(),
        [&](const auto& kv) {
            return (is_buy  && kv.second.qty < 0) ||
                   (!is_buy && kv.second.qty > 0);
        });

    if (it != open_pos_.end()) {
        // Закрытие есть: считаем pnl
        const double pnl = (upd.price - it->second.entry_price)
                           * upd.qty_filled
                           * (it->second.qty > 0 ? 1.0 : -1.0);
        state_.daily_pnl    += pnl;
        state_.liquid_value += pnl;
        if (state_.liquid_value > peak_liquid_)
            peak_liquid_ = state_.liquid_value;
        spdlog::debug("[Risk] on_fill CLOSE tid={} pnl={:+.1f} daily_pnl={:.1f}",
            tid, pnl, state_.daily_pnl);
        open_pos_.erase(it);
    } else {
        // Открытие новой позиции — запоминаем цену входа
        open_pos_[tid] = OpenPos{
            .entry_price = upd.price,
            .qty         = is_buy ? upd.qty_filled : -upd.qty_filled,
        };
        spdlog::debug("[Risk] on_fill OPEN tid={} price={} qty={}",
            tid, upd.price, upd.qty_filled);
    }
}

// ── circuit breaker ────────────────────────────────────────────────────────────────

void RiskManager::trip_circuit_breaker(std::string_view reason) noexcept {
    if (tripped_.exchange(true, std::memory_order_acq_rel)) return;
    { std::unique_lock lock(mu_); trip_reason_ = std::string(reason); }
    spdlog::critical("[Risk] CIRCUIT BREAKER TRIPPED: {}", reason);
}

bool RiskManager::is_tripped() const noexcept {
    return tripped_.load(std::memory_order_acquire);
}

AccountState RiskManager::account_state() const {
    std::shared_lock lock(mu_);
    return state_;
}

// ── fetch_initial_margin ────────────────────────────────────────────────────────────

Result<double> RiskManager::fetch_initial_margin(
    const Symbol& sym, bool is_long) const
{
    auto stub = proto_assets::AssetsService::NewStub(token_mgr_->channel());
    grpc::ClientContext ctx;
    ctx.AddMetadata("authorization", "Bearer " + token_mgr_->jwt());

    proto_assets::GetTradingInfoRequest req;
    req.set_symbol(sym.to_string());
    req.set_account_id(std::string(token_mgr_->primary_account_id()));

    proto_assets::GetTradingInfoResponse resp;
    const auto status = stub->GetTradingInfo(&ctx, req, &resp);
    if (!status.ok()) {
        spdlog::warn("[Risk] GetTradingInfo failed for {}: {}",
            sym.to_string(), status.error_message());
        return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    }
    return is_long
        ? decimal_to_double(resp.long_initial_margin())
        : decimal_to_double(resp.short_initial_margin());
}

// ── check ─────────────────────────────────────────────────────────────────────────────

Result<void> RiskManager::check(const OrderRequest& req) {
    if (auto r = check_circuit_breaker(); !r) return r;
    if (auto r = check_daily_loss();      !r) return r;
    if (auto r = check_drawdown();        !r) return r;
    if (auto r = check_position_count();  !r) return r;
    if (auto r = check_expiry(req.symbol);!r) return r;
    if (auto r = check_margin(req);       !r) return r;
    return {};
}

Result<void> RiskManager::check_circuit_breaker() const noexcept {
    if (!tripped_.load(std::memory_order_acquire)) return {};
    std::shared_lock lock(mu_);
    return std::unexpected(Error{
        ErrorCode::RiskLimitExceeded,
        "circuit breaker tripped: " + trip_reason_
    });
}

Result<void> RiskManager::check_daily_loss() const noexcept {
    std::shared_lock lock(mu_);
    if (state_.liquid_value < 1.0) return {};
    const double loss_pct = -state_.daily_pnl / state_.liquid_value * 100.0;
    if (loss_pct >= cfg_.max_daily_loss_pct) {
        spdlog::warn("[Risk] daily loss {:.2f}% >= limit {:.2f}%",
            loss_pct, cfg_.max_daily_loss_pct);
        return std::unexpected(Error{ErrorCode::DailyLossLimitHit,
            "daily loss limit hit"});
    }
    return {};
}

Result<void> RiskManager::check_drawdown() const noexcept {
    std::shared_lock lock(mu_);
    if (peak_liquid_ < 1.0) return {};
    const double dd_pct =
        (peak_liquid_ - state_.liquid_value) / peak_liquid_ * 100.0;
    if (dd_pct >= cfg_.max_drawdown_pct) {
        spdlog::warn("[Risk] drawdown {:.2f}% >= limit {:.2f}%",
            dd_pct, cfg_.max_drawdown_pct);
        return std::unexpected(Error{ErrorCode::RiskLimitExceeded,
            "max drawdown exceeded"});
    }
    return {};
}

Result<void> RiskManager::check_position_count() const noexcept {
    std::shared_lock lock(mu_);
    if (state_.open_positions >= static_cast<double>(cfg_.max_positions)) {
        spdlog::warn("[Risk] open positions {:.0f} >= limit {}",
            state_.open_positions, cfg_.max_positions);
        return std::unexpected(Error{ErrorCode::RiskLimitExceeded,
            "max positions reached"});
    }
    return {};
}

Result<void> RiskManager::check_margin(const OrderRequest& req) {
    const bool is_long = req.side == OrderSide::Buy;
    auto margin_r = fetch_initial_margin(req.symbol, is_long);
    if (!margin_r) {
        spdlog::warn("[Risk] cannot fetch margin for {}, skipping",
            req.symbol.to_string());
        return {};
    }
    const double required = *margin_r * req.quantity;
    std::shared_lock lock(mu_);
    if (required > state_.free_margin) {
        spdlog::warn("[Risk] insufficient margin: need {:.0f}, free {:.0f}",
            required, state_.free_margin);
        return std::unexpected(Error{ErrorCode::InsufficientMargin,
            "insufficient margin"});
    }
    if (state_.liquid_value > 1.0) {
        const double max_allowed =
            state_.liquid_value * cfg_.per_trade_pct / 100.0;
        if (required > max_allowed) {
            spdlog::warn("[Risk] per-trade {:.0f} > {:.1f}% limit ({:.0f})",
                required, cfg_.per_trade_pct, max_allowed);
            return std::unexpected(Error{ErrorCode::RiskLimitExceeded,
                "per-trade margin limit exceeded"});
        }
    }
    return {};
}

// ── check_expiry ───────────────────────────────────────────────────────────────────
//
// Используем core::expiry_day() — третья пятница реального месяца.
// rollover_days до expiry_day — блокируем новые входы.

Result<void> RiskManager::check_expiry(const Symbol& sym) const noexcept {
    const int ed = core::expiry_day(sym);  // third friday day-of-month
    if (ed == 0) return {};  // не удалось распарсить — пропускаем

    // Извлекаем год/месяц экспирации из символа
    const auto& code  = sym.security_code;
    const auto  dash  = code.find('-');
    const auto  dot   = code.find('.');
    if (dash == std::string::npos || dot == std::string::npos) return {};
    int exp_month = 0, exp_year = 0;
    try {
        exp_month = std::stoi(code.substr(dash + 1, dot - dash - 1));
        exp_year  = 2000 + std::stoi(code.substr(dot + 1));
    } catch (...) { return {}; }

    const auto now = std::chrono::system_clock::now();
    const auto tt  = std::chrono::system_clock::to_time_t(now);
    struct tm tm_now{};
    gmtime_r(&tt, &tm_now);
    const int cur_year  = tm_now.tm_year + 1900;
    const int cur_month = tm_now.tm_mon + 1;
    const int cur_day   = tm_now.tm_mday;

    // Контракт уже истёк: запрещаем
    if (cur_year > exp_year ||
        (cur_year == exp_year && cur_month > exp_month))
    {
        return std::unexpected(Error{ErrorCode::RiskLimitExceeded,
            "contract expired: " + sym.to_string()});
    }

    // Зона ролловера: cur_month == exp_month && cur_day >= ed - rollover_days
    if (cur_year == exp_year && cur_month == exp_month &&
        cur_day >= ed - cfg_.rollover_days)
    {
        spdlog::warn("[Risk] {} enters rollover window (expiry day {})",
            sym.to_string(), ed);
        return std::unexpected(Error{ErrorCode::RiskLimitExceeded,
            "rollover window: switch to next contract"});
    }
    return {};
}

} // namespace finam::risk
