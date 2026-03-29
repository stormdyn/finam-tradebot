#pragma once
#include <string>
#include <string_view>
#include <expected>
#include <functional>
#include <chrono>
#include <cstdint>

namespace finam {

// ─── Error types ─────────────────────────────────────────────────────────────

enum class ErrorCode : uint32_t {
    Ok = 0,
    // Auth
    AuthFailed,
    JwtExpired,
    JwtRefreshFailed,
    // Network
    ConnectionFailed,
    StreamDisconnected,
    Timeout,
    // Order
    OrderRejected,
    InsufficientMargin,
    InvalidPrice,
    // Risk
    RiskLimitExceeded,
    DailyLossLimitHit,
    // Generic
    InvalidArgument,
    NotImplemented,
    Internal,
};

struct Error {
    ErrorCode   code;
    std::string message;

    [[nodiscard]] static Error ok() noexcept {
        return {ErrorCode::Ok, {}};
    }
    [[nodiscard]] bool is_ok() const noexcept { return code == ErrorCode::Ok; }
};

template<typename T>
using Result = std::expected<T, Error>;

// ─── Market data events ───────────────────────────────────────────────────────

using Timestamp = std::chrono::system_clock::time_point;

struct Quote {
    std::string symbol;
    double      bid{};
    double      ask{};
    double      last{};
    int64_t     volume{};
    Timestamp   ts;
};

struct Bar {
    std::string symbol;
    double      open{}, high{}, low{}, close{};
    int64_t     volume{};
    Timestamp   ts;
};

// ─── Order/execution events ───────────────────────────────────────────────────

enum class OrderSide   { Buy, Sell };
enum class OrderStatus { Pending, PartialFill, Filled, Cancelled, Rejected };

struct OrderUpdate {
    std::string  order_id;
    std::string  symbol;
    OrderSide    side{};
    OrderStatus  status{};
    double       price{};
    int64_t      qty_total{};
    int64_t      qty_filled{};
    Timestamp    ts;
};

// ─── Strategy signal ──────────────────────────────────────────────────────────

struct Signal {
    enum class Direction { Buy, Sell, Close, None };
    std::string symbol;
    Direction   direction{Direction::None};
    double      price{};      // 0 = market order
    int64_t     quantity{};
    std::string reason;       // human-readable, for logs
};

// ─── Interfaces ───────────────────────────────────────────────────────────────

class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Called on each new bar (e.g. 1-min candle)
    virtual Signal on_bar(const Bar& bar) = 0;

    // Called on each L1 quote update
    virtual Signal on_quote(const Quote& quote) = 0;

    // Called when an order status changes
    virtual void on_order_update(const OrderUpdate& update) = 0;

    // Strategy name for logging
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

struct OrderRequest {
    std::string  account_id;
    std::string  symbol;
    OrderSide    side{};
    double       price{};     // 0 = market
    int64_t      quantity{};
    bool         is_market{false};
};

class IOrderExecutor {
public:
    virtual ~IOrderExecutor() = default;
    virtual Result<std::string> submit(const OrderRequest& req) = 0;
    virtual Result<void>        cancel(std::string_view order_id) = 0;
};

class IRiskManager {
public:
    virtual ~IRiskManager() = default;

    // Returns Ok or RiskLimitExceeded / InsufficientMargin
    virtual Result<void> check(const OrderRequest& req) = 0;
};

} // namespace finam
