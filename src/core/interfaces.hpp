#pragma once
#include <string>
#include <string_view>
#include <expected>
#include <chrono>
#include <cstdint>
#include <vector>

namespace finam {

// ── Ошибки ────────────────────────────────────────────────────────────────────

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

// ── Базовые типы ──────────────────────────────────────────────────────────────

using Timestamp = std::chrono::system_clock::time_point;

// Идентификатор инструмента: формат API v2 — "{TICKER}-{MM}.{YY}@{MIC}"
// Пример: Si-6.26@FORTS, RTS-6.26@FORTS
// security_code = "Si-6.26", security_board = "FORTS"
struct Symbol {
    std::string security_code;   // "Si-6.26", "RTS-6.26", "GOLD-6.26"
    std::string security_board;  // "FORTS" для срочного рынка MOEX

    // Возвращает строку в формате API: "Si-6.26@FORTS"
    [[nodiscard]] std::string to_string() const {
        return security_code + "@" + security_board;
    }
    [[nodiscard]] bool operator==(const Symbol&) const = default;
};

// ── Рыночные данные ───────────────────────────────────────────────────────────

struct Quote {
    Symbol    symbol;
    double    bid{};
    double    ask{};
    double    last{};
    int64_t   volume{};
    Timestamp ts;
};

struct Bar {
    Symbol    symbol;
    double    open{}, high{}, low{}, close{};
    int64_t   volume{};
    Timestamp ts;
};

struct OrderBookRow {
    double  price{};
    int64_t quantity{};
};

struct OrderBook {
    Symbol                    symbol;
    std::vector<OrderBookRow> asks;  // по возрастанию цены
    std::vector<OrderBookRow> bids;  // по убыванию цены
    Timestamp                 ts;
};

// ── Ордера ────────────────────────────────────────────────────────────────────

enum class OrderSide   { Buy, Sell };
enum class OrderStatus { Pending, PartialFill, Filled, Cancelled, Rejected };
enum class OrderType   { Market, Limit };

struct OrderUpdate {
    int64_t     order_no{};       // биржевой номер
    int32_t     transaction_id{}; // наш локальный ID
    Symbol      symbol;
    std::string client_id;        // торговый счёт
    OrderSide   side{};
    OrderStatus status{};
    OrderType   type{};
    double      price{};
    int32_t     qty_total{};
    int32_t     qty_filled{};
    std::string message;          // причина отказа если Rejected
    Timestamp   ts;
};

// ── Сигнал стратегии ──────────────────────────────────────────────────────────

struct Signal {
    enum class Direction { Buy, Sell, Close, None };

    Symbol      symbol;
    Direction   direction{Direction::None};
    OrderType   order_type{OrderType::Market};
    double      price{};      // игнорируется при Market
    int32_t     quantity{};
    std::string reason;       // для логов
};

// ── Запрос на выставление ордера ──────────────────────────────────────────────

struct OrderRequest {
    std::string client_id;    // торговый счёт
    Symbol      symbol;
    OrderSide   side{};
    OrderType   type{OrderType::Market};
    double      price{};      // только для Limit
    int32_t     quantity{};
};

// ── Интерфейсы компонентов ────────────────────────────────────────────────────

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

    // Возвращает transaction_id
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
