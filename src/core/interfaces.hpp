#pragma once
#include <string>
#include <string_view>
#include <expected>
#include <chrono>
#include <cstdint>
#include <vector>

namespace finam {

// ── Ошибки ──────────────────────────────────────────────────────────────────────

enum class ErrorCode : uint32_t {
    Ok = 0,
    // Network / gRPC
    ConnectionFailed,
    StreamDisconnected,
    Timeout,
    RpcError,
    // Order
    OrderRejected,
    InsufficientMargin,
    InvalidPrice,
    InvalidQuantity,
    // Risk
    RiskLimitExceeded,
    DailyLossLimitHit,
    // Generic
    InvalidArgument,
    NotImplemented,
    Internal,
};

struct Error {
    ErrorCode   code{ErrorCode::Ok};
    std::string message;

    [[nodiscard]] static Error ok() noexcept { return {}; }
    [[nodiscard]] bool is_ok() const noexcept { return code == ErrorCode::Ok; }
};

template<typename T>
using Result = std::expected<T, Error>;

// ── Базовые типы ───────────────────────────────────────────────────────────────────

using Timestamp = std::chrono::system_clock::time_point;

// Идентификатор инструмента: формат API v2 — "{TICKER}-{MM}.{YY}@{MIC}"
struct Symbol {
    std::string security_code;   // "Si-6.26", "RTS-6.26", "GOLD-6.26"
    std::string security_board;  // "FORTS" для срочного рынка MOEX

    [[nodiscard]] std::string to_string() const {
        return security_code + "@" + security_board;
    }
    [[nodiscard]] bool operator==(const Symbol&) const = default;
};

// ── Рыночные данные ────────────────────────────────────────────────────────────────

struct Quote {
    Symbol    symbol;
    double    bid{};
    double    ask{};
    double    last{};
    int64_t   volume{};
    Timestamp ts;
};

// Bar содержит таймфрейм чтобы стратегия могла различать D1 от M1
// Без этого поля detect_timeframe() был невозможен без heuristic по duration
struct Bar {
    Symbol      symbol;
    std::string timeframe;  // "D1", "M1", "M5", "H1" — заполняет MarketDataClient
    double      open{}, high{}, low{}, close{};
    int64_t     volume{};
    Timestamp   ts;
    // date: используется в backtest для человеческого лога/CSV (заполняет load_csv)
    std::string date;
};

struct OrderBookRow {
    double  price{};
    int64_t quantity{};
};

struct OrderBook {
    Symbol                    symbol;
    std::vector<OrderBookRow> asks;
    std::vector<OrderBookRow> bids;
    Timestamp                 ts;
};

// ── Ордера ──────────────────────────────────────────────────────────────────────

enum class OrderSide   { Buy, Sell };
enum class OrderStatus { Pending, PartialFill, Filled, Cancelled, Rejected };
enum class OrderType   { Market, Limit };

struct OrderUpdate {
    int64_t     order_no{};
    int32_t     transaction_id{};
    Symbol      symbol;
    std::string client_id;
    OrderSide   side{};
    OrderStatus status{};
    OrderType   type{};
    double      price{};
    int32_t     qty_total{};
    int32_t     qty_filled{};
    std::string message;
    Timestamp   ts;
};

// ── Сигнал ──────────────────────────────────────────────────────────────────────

struct Signal {
    enum class Direction { Buy, Sell, Close, None };

    Symbol      symbol;
    Direction   direction{Direction::None};
    OrderType   order_type{OrderType::Market};
    double      price{};
    int32_t     quantity{};
    std::string reason;
};

// ── Запрос на выставление ордера ───────────────────────────────────────────────

struct OrderRequest {
    std::string client_id;
    Symbol      symbol;
    OrderSide   side{};
    OrderType   type{OrderType::Market};
    double      price{};
    int32_t     quantity{};
};

// ── Интерфейсы ───────────────────────────────────────────────────────────────────

class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual Signal on_bar(const Bar& bar)                  = 0;
    virtual Signal on_quote(const Quote& quote)            = 0;
    virtual void   on_order_update(const OrderUpdate& upd) = 0;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

class IOrderExecutor {
public:
    virtual ~IOrderExecutor() = default;

    virtual Result<int32_t> submit(const OrderRequest& req)    = 0;
    virtual Result<void>    cancel(int64_t order_no,
                                   std::string_view client_id) = 0;
};

class IRiskManager {
public:
    virtual ~IRiskManager() = default;
    virtual Result<void> check(const OrderRequest& req) = 0;
};

} // namespace finam
