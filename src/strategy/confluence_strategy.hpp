#pragma once
#include <optional>
#include <string_view>
#include <spdlog/spdlog.h>

#include "core/interfaces.hpp"
#include "ofi_types.hpp"
#include "order_book_state.hpp"
#include "trade_flow_analyzer.hpp"
#include "spoof_filter.hpp"
#include "session_context.hpp"

namespace finam::strategy {

// ── ConfluenceStrategy ────────────────────────────────────────────────────
//
// Логика входа (все условия AND):
//   [1] orb_finalized && bias != None
//   [2] mlofi.vote() == tfi.vote()          (оба согласны)
//   [3] ofi_vote.is_strong()                (confluence == ±2)
//   [4] vote направление == ORB bias
//   [5] !spoof_filter.is_spoofed(bbo)
//
// Логика выхода:
//   [A] Противоположный сильный сигнал
//   [B] pnl < -sl_ticks  или  pnl > tp_ticks
//
// Размер: base_qty * session_context.size_multiplier()
// THREADING: все методы из одного strategy thread.

class ConfluenceStrategy final : public IStrategy {
public:
    struct Config {
        Symbol   symbol;
        int32_t  base_qty{1};
        double   sl_ticks{30.0};
        double   tp_ticks{90.0};
        double   tick_size{1.0};

        OrderBookState::Config    ob_cfg;
        TradeFlowAnalyzer::Config tfi_cfg;
        SpoofFilter::Config       spoof_cfg;
        SessionContext::Config    session_cfg;
    };

    explicit ConfluenceStrategy(Config cfg)
        : cfg_(cfg)
        , ob_state_(cfg.ob_cfg)
        , tfi_analyzer_(cfg.tfi_cfg)
        , spoof_filter_(cfg.spoof_cfg)
        , session_ctx_(cfg.session_cfg)
    {}

    // ── IStrategy ─────────────────────────────────────────────────────────

    Signal on_bar(const Bar& bar) override {
        if (bar.symbol != cfg_.symbol) return no_signal();
        const auto tf = detect_timeframe(bar);
        if (tf == Timeframe::Daily)
            session_ctx_.on_daily_bar(bar);
        else if (tf == Timeframe::Intraday) {
            handle_session_open(bar.ts);
            session_ctx_.on_intraday_bar(bar);
        }
        return no_signal();
    }

    Signal on_quote(const Quote& quote) override {
        if (quote.symbol != cfg_.symbol) return no_signal();
        const double mid = (quote.bid + quote.ask) * 0.5;
        handle_session_open(quote.ts);

        if (!session_ctx_.orb_finalized())
            session_ctx_.finalize_orb(mid);

        if (position_ != 0) {
            if (auto exit = check_exit(mid, quote.ts))
                return *exit;
        }
        return no_signal();
    }

    void on_order_update(const OrderUpdate& upd) override {
        if (upd.status == OrderStatus::Filled ||
            upd.status == OrderStatus::PartialFill)
        {
            spdlog::info("[Confluence] fill: side={} qty={} price={}",
                upd.side == OrderSide::Buy ? "BUY" : "SELL",
                upd.qty_filled, upd.price);
        }
        if (upd.status == OrderStatus::Filled) {
            if (position_ == 0) entry_price_ = upd.price;
            position_ += (upd.side == OrderSide::Buy)
                         ? upd.qty_filled : -upd.qty_filled;
        }
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "ConfluenceStrategy";
    }

    [[nodiscard]] const Symbol& symbol() const noexcept { return cfg_.symbol; }

    // ── OFI event pipeline (из MD consumer) ──────────────────────────────

    std::optional<Signal> on_book_event(const BookLevelEvent& e) {
        update_spoof_on_book(e);
        ob_state_.on_book_event(e);
        return try_enter(e.ts, current_mid_);
    }

    std::optional<Signal> on_trade_event(const TradeEvent& e) {
        current_mid_ = e.price;
        tfi_analyzer_.on_trade(e);
        return try_enter(e.ts, e.price);
    }

    void update_bbo(double bid, double ask) noexcept {
        current_bid_ = bid;
        current_ask_ = ask;
        current_mid_ = (bid + ask) * 0.5;
    }

    void on_session_open() noexcept {
        session_ctx_.session_reset();
        tfi_analyzer_.session_reset();
        spoof_filter_.session_reset();
        ob_state_.reset();
        position_       = 0;
        entry_price_    = 0.0;
        last_signal_ts_ = {};
        spdlog::info("[Confluence] session reset");
    }

private:
    enum class Timeframe { Daily, Intraday, Unknown };

    std::optional<Signal> try_enter(
        std::chrono::system_clock::time_point ts,
        double price) noexcept
    {
        if (position_ != 0)                    return std::nullopt;
        if (!session_ctx_.orb_finalized())     return std::nullopt;
        if (session_ctx_.bias() == SessionContext::Bias::None)
                                               return std::nullopt;
        if (ts - last_signal_ts_ < std::chrono::seconds(5))
                                               return std::nullopt;

        const Vote mlofi_vote = ob_state_.vote();
        const Vote tfi_vote   = tfi_analyzer_.vote();
        const OfiVote vote{
            .mlofi      = mlofi_vote,
            .tfi        = tfi_vote,
            .confluence = static_cast<int8_t>(
                static_cast<int>(mlofi_vote) + static_cast<int>(tfi_vote))
        };

        if (!vote.is_strong()) return std::nullopt;

        if (vote.is_long()  && !session_ctx_.allows_long())  return std::nullopt;
        if (vote.is_short() && !session_ctx_.allows_short()) return std::nullopt;

        if (spoof_filter_.is_spoofed(current_bid_, ts) ||
            spoof_filter_.is_spoofed(current_ask_, ts))
            return std::nullopt;

        last_signal_ts_ = ts;
        return build_entry(vote, price);
    }

    Signal build_entry(const OfiVote& vote, double price) noexcept {
        const bool is_long = vote.is_long();
        const int32_t qty  = static_cast<int32_t>(
            cfg_.base_qty * session_ctx_.size_multiplier());

        spdlog::info("[Confluence] ENTRY {} qty={} price={:.2f} nr7={} mlofi={} tfi={}",
            is_long ? "LONG" : "SHORT", qty, price,
            session_ctx_.nr7_confirmed(),
            static_cast<int>(vote.mlofi),
            static_cast<int>(vote.tfi));

        Signal sig;
        sig.symbol     = cfg_.symbol;
        sig.direction  = is_long ? Signal::Direction::Buy
                                 : Signal::Direction::Sell;
        sig.order_type = OrderType::Market;
        sig.quantity   = qty;
        sig.reason     = is_long ? "confluence_long" : "confluence_short";
        return sig;
    }

    std::optional<Signal> check_exit(
        double price,
        std::chrono::system_clock::time_point ts) noexcept
    {
        if (position_ == 0 || entry_price_ < 1e-9) return std::nullopt;

        const double pnl_ticks = (position_ > 0)
            ? (price - entry_price_) / cfg_.tick_size
            : (entry_price_ - price) / cfg_.tick_size;

        if (pnl_ticks < -cfg_.sl_ticks) {
            spdlog::warn("[Confluence] SL hit pnl={:.1f}", pnl_ticks);
            return close_signal("sl_hit");
        }
        if (pnl_ticks > cfg_.tp_ticks) {
            spdlog::info("[Confluence] TP hit pnl={:.1f}", pnl_ticks);
            return close_signal("tp_hit");
        }

        const int8_t conf = static_cast<int8_t>(
            static_cast<int>(ob_state_.vote()) +
            static_cast<int>(tfi_analyzer_.vote()));

        if ((position_ > 0 && conf == -2) || (position_ < 0 && conf == 2)) {
            spdlog::info("[Confluence] flow reversal exit");
            return close_signal("flow_reversal");
        }
        (void)ts;
        return std::nullopt;
    }

    Signal close_signal(std::string_view reason) noexcept {
        Signal sig;
        sig.symbol     = cfg_.symbol;
        sig.direction  = Signal::Direction::Close;
        sig.order_type = OrderType::Market;
        sig.quantity   = std::abs(position_);
        sig.reason     = std::string(reason);
        return sig;
    }

    void update_spoof_on_book(const BookLevelEvent& e) noexcept {
        const double added_bid = e.new_bid_size - e.old_bid_size;
        const double added_ask = e.new_ask_size - e.old_ask_size;
        if (added_bid > cfg_.spoof_cfg.min_large_qty)
            spoof_filter_.on_large_add(e.price, e.ts);
        if (added_ask > cfg_.spoof_cfg.min_large_qty)
            spoof_filter_.on_large_add(e.price, e.ts);
        if (e.old_bid_size > cfg_.spoof_cfg.min_large_qty && e.new_bid_size < 1e-9)
            spoof_filter_.on_large_cancel(e.price, e.ts);
        if (e.old_ask_size > cfg_.spoof_cfg.min_large_qty && e.new_ask_size < 1e-9)
            spoof_filter_.on_large_cancel(e.price, e.ts);
    }

    void handle_session_open(
        std::chrono::system_clock::time_point ts) noexcept
    {
        const int min_utc = session_open_utc_min(ts);
        const int open    = cfg_.session_cfg.session_open_utc_min;
        if (!session_opened_today_ && min_utc >= open && min_utc < open + 5) {
            session_opened_today_ = true;
            on_session_open();
        }
        if (min_utc < open) session_opened_today_ = false;
    }

    [[nodiscard]] static int session_open_utc_min(
        std::chrono::system_clock::time_point ts) noexcept
    {
        const auto tt = std::chrono::system_clock::to_time_t(ts);
        return static_cast<int>((tt % 86400) / 60);
    }

    [[nodiscard]] Timeframe detect_timeframe(const Bar&) const noexcept {
        return Timeframe::Intraday; // TODO: разделить D1/M1 через StrategyRunner
    }

    [[nodiscard]] static Signal no_signal() noexcept {
        return Signal{.direction = Signal::Direction::None};
    }

    // ── State ─────────────────────────────────────────────────────────────

    Config            cfg_;
    OrderBookState    ob_state_;
    TradeFlowAnalyzer tfi_analyzer_;
    SpoofFilter       spoof_filter_;
    SessionContext    session_ctx_;

    int32_t  position_{0};
    double   entry_price_{0.0};
    double   current_bid_{0.0};
    double   current_ask_{0.0};
    double   current_mid_{0.0};
    bool     session_opened_today_{false};

    std::chrono::system_clock::time_point last_signal_ts_{};
};

} // namespace finam::strategyф