#include "order_client.hpp"
#include "core/grpc_fmt.hpp"
#include "core/backoff.hpp"
#include "core/maintenance.hpp"
#include <spdlog/spdlog.h>
#include "grpc/tradeapi/v1/orders/orders_service.grpc.pb.h"

namespace proto_orders = ::grpc::tradeapi::v1::orders;

namespace finam::order {

namespace {

std::optional<Error> validate(const OrderRequest& req) {
    if (req.client_id.empty())
        return Error{ErrorCode::InvalidArgument, "client_id is empty"};
    if (req.quantity <= 0)
        return Error{ErrorCode::InvalidQuantity,
            "quantity must be > 0, got " + std::to_string(req.quantity)};
    if (req.type == OrderType::Limit && req.price <= 0.0)
        return Error{ErrorCode::InvalidPrice, "limit order requires price > 0"};
    return std::nullopt;
}

void set_decimal(google::type::Decimal* d, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    d->set_value(buf);
}

// Полная таблица статусов из proto (orders_service.proto).
// Дефолт на Pending был неправильным: EXPIRED/FAILED/DONE_FOR_DAY
// трактовались как живые ордера, что ломало логику стратегии.
OrderStatus map_status(proto_orders::OrderStatus s) {
    using PS = proto_orders::OrderStatus;
    switch (s) {
        // Живые
        case PS::ORDER_STATUS_NEW:           return OrderStatus::Pending;
        case PS::ORDER_STATUS_PENDING_NEW:   return OrderStatus::Pending;
        case PS::ORDER_STATUS_PENDING_CANCEL:return OrderStatus::Pending;
        case PS::ORDER_STATUS_SUSPENDED:     return OrderStatus::Pending;
        case PS::ORDER_STATUS_WATCHING:      return OrderStatus::Pending;
        case PS::ORDER_STATUS_WAIT:          return OrderStatus::Pending;
        case PS::ORDER_STATUS_LINK_WAIT:     return OrderStatus::Pending;
        case PS::ORDER_STATUS_FORWARDING:    return OrderStatus::Pending;
        case PS::ORDER_STATUS_SL_FORWARDING: return OrderStatus::Pending;
        case PS::ORDER_STATUS_TP_FORWARDING: return OrderStatus::Pending;
        case PS::ORDER_STATUS_SL_GUARD_TIME: return OrderStatus::Pending;
        case PS::ORDER_STATUS_TP_GUARD_TIME: return OrderStatus::Pending;
        case PS::ORDER_STATUS_TP_CORRECTION: return OrderStatus::Pending;
        case PS::ORDER_STATUS_TP_CORR_GUARD_TIME: return OrderStatus::Pending;
        // Частичное исполнение
        case PS::ORDER_STATUS_PARTIALLY_FILLED: return OrderStatus::PartialFill;
        // Полное исполнение
        case PS::ORDER_STATUS_FILLED:        return OrderStatus::Filled;
        case PS::ORDER_STATUS_EXECUTED:      return OrderStatus::Filled;
        case PS::ORDER_STATUS_SL_EXECUTED:   return OrderStatus::Filled;
        case PS::ORDER_STATUS_TP_EXECUTED:   return OrderStatus::Filled;
        // Отменены / завершены
        case PS::ORDER_STATUS_CANCELED:      return OrderStatus::Cancelled;
        case PS::ORDER_STATUS_REPLACED:      return OrderStatus::Cancelled;
        case PS::ORDER_STATUS_DONE_FOR_DAY:  return OrderStatus::Cancelled;
        case PS::ORDER_STATUS_EXPIRED:       return OrderStatus::Cancelled;
        case PS::ORDER_STATUS_DISABLED:      return OrderStatus::Cancelled;
        // Отклонены
        case PS::ORDER_STATUS_REJECTED:            return OrderStatus::Rejected;
        case PS::ORDER_STATUS_REJECTED_BY_EXCHANGE:return OrderStatus::Rejected;
        case PS::ORDER_STATUS_DENIED_BY_BROKER:    return OrderStatus::Rejected;
        case PS::ORDER_STATUS_FAILED:              return OrderStatus::Rejected;
        // Неизвестные новые статусы — оставляем как Pending
        // и пишем предупреждение, чтобы заметить в логах
        default:
            spdlog::warn("[Order] unknown OrderStatus={}, treating as Pending",
                static_cast<int>(s));
            return OrderStatus::Pending;
    }
}

} // namespace

// ── OrderClient ────────────────────────────────────────────────────────────────────

OrderClient::OrderClient(
    std::shared_ptr<auth::TokenManager> token_mgr,
    std::string                         account_id,
    OrderUpdateCallback                 on_update)
    : token_mgr_(std::move(token_mgr))
    , account_id_(std::move(account_id))
    , on_update_(std::move(on_update))
    , orders_stub_(proto_orders::OrdersService::NewStub(token_mgr_->channel()))
{
    stream_thread_ = std::thread(&OrderClient::run_order_stream, this);
    spdlog::debug("[Order] OrderClient created, account={}", account_id_);
}

OrderClient::~OrderClient() { shutdown(); }

void OrderClient::shutdown() noexcept {
    if (stop_.exchange(true, std::memory_order_acq_rel)) return;
    if (stream_thread_.joinable()) stream_thread_.join();
    spdlog::debug("[Order] OrderClient destroyed");
}

std::unique_ptr<grpc::ClientContext> OrderClient::make_context() const {
    auto ctx = std::make_unique<grpc::ClientContext>();
    ctx->AddMetadata("authorization", "Bearer " + token_mgr_->jwt());
    return ctx;
}

int64_t OrderClient::next_id() noexcept {
    return id_counter_.fetch_add(1, std::memory_order_relaxed);
}

// ── submit ──────────────────────────────────────────────────────────────────────────

Result<int64_t> OrderClient::submit(const OrderRequest& req) {
    if (auto err = validate(req))
        return std::unexpected(*err);

    const int64_t local_id = next_id();
    const auto    sym      = req.symbol.to_string();

    spdlog::info("[Order] submit local_id={} symbol={} side={} type={} qty={} price={}",
        local_id, sym,
        req.side == OrderSide::Buy ? "BUY" : "SELL",
        req.type == OrderType::Market ? "MARKET" : "LIMIT",
        req.quantity, req.price);

    proto_orders::Order grpc_req;
    grpc_req.set_account_id(account_id_);
    grpc_req.set_symbol(sym);
    set_decimal(grpc_req.mutable_quantity(), static_cast<double>(req.quantity));
    grpc_req.set_side(req.side == OrderSide::Buy
        ? ::grpc::tradeapi::v1::SIDE_BUY
        : ::grpc::tradeapi::v1::SIDE_SELL);
    grpc_req.set_client_order_id(std::to_string(local_id));

    if (req.type == OrderType::Market) {
        grpc_req.set_type(proto_orders::ORDER_TYPE_MARKET);
        grpc_req.set_time_in_force(proto_orders::TIME_IN_FORCE_IOC);
    } else {
        grpc_req.set_type(proto_orders::ORDER_TYPE_LIMIT);
        grpc_req.set_time_in_force(proto_orders::TIME_IN_FORCE_DAY);
        set_decimal(grpc_req.mutable_limit_price(), req.price);
    }

    proto_orders::OrderState resp;
    auto ctx = make_context();
    const auto status = orders_stub_->PlaceOrder(ctx.get(), grpc_req, &resp);

    if (!status.ok()) {
        spdlog::error("[Order] PlaceOrder failed: code={} msg={}",
            status.error_code(), status.error_message());
        return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    }

    upsert(OrderState{
        .order_id      = resp.order_id(),
        .local_id      = local_id,
        .symbol        = req.symbol,
        .side          = req.side,
        .status        = map_status(resp.status()),
        .type          = req.type,
        .price         = req.price,
        .qty_total     = req.quantity,
        .reject_reason = {},
        .ts            = std::chrono::system_clock::now(),
    });

    spdlog::info("[Order] PlaceOrder ok order_id={}", resp.order_id());
    return local_id;
}

// ── cancel ──────────────────────────────────────────────────────────────────────────

Result<void> OrderClient::cancel(int64_t local_id, std::string_view) {
    // CancelOrderRequest.order_id — биржевой string ID из OrderState.order_id,
    // НЕ наш локальный счётчик.
    std::string exchange_order_id;
    {
        std::shared_lock lock(orders_mu_);
        auto it = orders_.find(local_id);
        if (it == orders_.end()) {
            spdlog::error("[Order] cancel: local_id={} not found in orders map", local_id);
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                "order local_id=" + std::to_string(local_id) + " not found"});
        }
        exchange_order_id = it->second.order_id;
    }

    if (exchange_order_id.empty()) {
        spdlog::error("[Order] cancel: local_id={} has empty exchange order_id", local_id);
        return std::unexpected(Error{ErrorCode::InvalidArgument, "exchange order_id is empty"});
    }

    spdlog::info("[Order] cancel local_id={} exchange_order_id={}", local_id, exchange_order_id);

    proto_orders::CancelOrderRequest req;
    req.set_account_id(account_id_);
    req.set_order_id(exchange_order_id);  // биржевой ID

    proto_orders::OrderState resp;
    auto ctx = make_context();
    const auto status = orders_stub_->CancelOrder(ctx.get(), req, &resp);

    if (!status.ok()) {
        spdlog::error("[Order] CancelOrder failed: code={} msg={}",
            status.error_code(), status.error_message());
        return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    }
    return {};
}

// ── active_orders / find ────────────────────────────────────────────────────────────

std::vector<OrderState> OrderClient::active_orders() const {
    std::shared_lock lock(orders_mu_);
    std::vector<OrderState> result;
    result.reserve(orders_.size());
    for (const auto& [_, state] : orders_)
        if (state.status == OrderStatus::Pending ||
            state.status == OrderStatus::PartialFill)
            result.push_back(state);
    return result;
}

std::optional<OrderState> OrderClient::find(int64_t local_id) const {
    std::shared_lock lock(orders_mu_);
    if (auto it = orders_.find(local_id); it != orders_.end())
        return it->second;
    return std::nullopt;
}

// ── upsert ──────────────────────────────────────────────────────────────────────────

void OrderClient::upsert(OrderState state) {
    const int64_t lid = state.local_id;

    int32_t prev_filled = 0;
    {
        std::shared_lock rlock(orders_mu_);
        if (auto it = orders_.find(lid); it != orders_.end())
            prev_filled = it->second.qty_filled;
    }
    const int32_t delta_filled = state.qty_filled - prev_filled;

    OrderUpdate upd{
        .order_no       = 0,
        .transaction_id = static_cast<int32_t>(lid),
        .symbol         = state.symbol,
        .client_id      = account_id_,
        .side           = state.side,
        .status         = state.status,
        .type           = state.type,
        .price          = state.price,
        .qty_total      = state.qty_total,
        .qty_filled     = delta_filled,
        .message        = state.reject_reason,
        .ts             = state.ts,
    };

    {
        std::unique_lock lock(orders_mu_);
        orders_[lid] = std::move(state);
    }

    invoke_callback(upd);
}

// ── run_order_stream ─────────────────────────────────────────────────────────────

void OrderClient::run_order_stream() {
    spdlog::info("[Order] order stream started account={}", account_id_);
    core::ExponentialBackoff  bo;
    core::MaintenanceWindow   maint;

    while (!stop_.load(std::memory_order_acquire)) {
        if (!maint.wait_if_active(stop_)) break;

        auto ctx = make_context();
        proto_orders::SubscribeOrdersRequest req;
        req.set_account_id(account_id_);
        auto stream = orders_stub_->SubscribeOrders(ctx.get(), req);

        proto_orders::SubscribeOrdersResponse resp;
        while (!stop_.load(std::memory_order_acquire) && stream->Read(&resp)) {
            for (const auto& o : resp.orders()) {
                int64_t local_id  = 0;
                int32_t qty_total = 0;
                int32_t filled    = 0;
                try { local_id  = std::stol(o.order().client_order_id()); } catch (...) {}
                try { qty_total = static_cast<int32_t>(std::stod(o.initial_quantity().value())); } catch (...) {}
                try { filled    = static_cast<int32_t>(std::stod(o.executed_quantity().value())); } catch (...) {}

                Symbol sym;
                {
                    std::shared_lock rlock(orders_mu_);
                    if (auto it = orders_.find(local_id); it != orders_.end())
                        sym = it->second.symbol;
                }

                upsert(OrderState{
                    .order_id      = o.order_id(),
                    .local_id      = local_id,
                    .symbol        = sym,
                    .side          = o.order().side() == ::grpc::tradeapi::v1::SIDE_BUY
                                     ? OrderSide::Buy : OrderSide::Sell,
                    .status        = map_status(o.status()),
                    .type          = o.order().type() == proto_orders::ORDER_TYPE_MARKET
                                     ? OrderType::Market : OrderType::Limit,
                    .price         = 0.0,
                    .qty_total     = qty_total,
                    .qty_filled    = filled,
                    .reject_reason = {},
                    .ts            = std::chrono::system_clock::now(),
                });
            }
            bo.reset();
        }
        stream->Finish();

        if (stop_.load(std::memory_order_acquire)) break;
        if (maint.is_active()) continue;

        spdlog::warn("[Order] stream ended, reconnect #{} with backoff", bo.attempt() + 1);
        if (!bo.wait(stop_)) break;
    }

    spdlog::info("[Order] order stream stopped");
}

} // namespace finam::order
