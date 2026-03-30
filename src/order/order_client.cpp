#include "order_client.hpp"
#include <spdlog/spdlog.h>
#include "grpc/tradeapi/v1/orders/orders_service.grpc.pb.h"

// Псевдонимы для читаемости
namespace proto_orders = ::grpc::tradeapi::v1::orders;

namespace finam::order {

// ── Helpers ───────────────────────────────────────────────────────────────────

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

// google.type.Decimal передаётся как строка в поле .value()
void set_decimal(google::type::Decimal* d, double v) {
    // Убираем trailing zeros через stringstream
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    d->set_value(buf);
}

// proto OrderStatus → наш OrderStatus
OrderStatus map_status(proto_orders::OrderStatus s) {
    using PS = proto_orders::OrderStatus;
    switch (s) {
        case PS::ORDER_STATUS_NEW:              return OrderStatus::Pending;
        case PS::ORDER_STATUS_PARTIALLY_FILLED: return OrderStatus::PartialFill;
        case PS::ORDER_STATUS_FILLED:           return OrderStatus::Filled;
        case PS::ORDER_STATUS_EXECUTED:         return OrderStatus::Filled;
        case PS::ORDER_STATUS_CANCELED:         return OrderStatus::Cancelled;
        case PS::ORDER_STATUS_REJECTED:
        case PS::ORDER_STATUS_REJECTED_BY_EXCHANGE:
        case PS::ORDER_STATUS_DENIED_BY_BROKER: return OrderStatus::Rejected;
        default:                                return OrderStatus::Pending;
    }
}

} // namespace

// ── OrderClient ───────────────────────────────────────────────────────────────

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

int32_t OrderClient::next_id() noexcept {
    return id_counter_.fetch_add(1, std::memory_order_relaxed);
}

// ── submit ────────────────────────────────────────────────────────────────────

Result<int32_t> OrderClient::submit(const OrderRequest& req) {
    if (auto err = validate(req))
        return std::unexpected(*err);

    const int32_t local_id = next_id();
    const auto    sym      = req.symbol.to_string();

    spdlog::info("[Order] submit local_id={} symbol={} side={} type={} qty={} price={}",
        local_id, sym,
        req.side == OrderSide::Buy ? "BUY" : "SELL",
        req.type == OrderType::Market ? "MARKET" : "LIMIT",
        req.quantity, req.price);

    // Собираем proto::Order
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
        spdlog::error("[Order] PlaceOrder failed: {} {}",
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

// ── cancel ────────────────────────────────────────────────────────────────────

Result<void> OrderClient::cancel(int64_t order_no, std::string_view client_id) {
    spdlog::info("[Order] cancel order_no={} client_id={}", order_no, client_id);

    proto_orders::CancelOrderRequest req;
    req.set_account_id(std::string(client_id));
    req.set_order_id(std::to_string(order_no));

    proto_orders::OrderState resp;
    auto ctx = make_context();
    const auto status = orders_stub_->CancelOrder(ctx.get(), req, &resp);

    if (!status.ok()) {
        spdlog::error("[Order] CancelOrder failed: {}", status.error_message());
        return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    }
    return {};
}

// ── active_orders / find ──────────────────────────────────────────────────────

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

std::optional<OrderState> OrderClient::find(int32_t local_id) const {
    std::shared_lock lock(orders_mu_);
    if (auto it = orders_.find(local_id); it != orders_.end())
        return it->second;
    return std::nullopt;
}

// ── upsert ────────────────────────────────────────────────────────────────────

void OrderClient::upsert(OrderState state) {
    const int32_t local_id = state.local_id;

    OrderUpdate upd{
        .order_no       = 0,
        .transaction_id = local_id,
        .symbol         = state.symbol,
        .client_id      = account_id_,
        .side           = state.side,
        .status         = state.status,
        .type           = state.type,
        .price          = state.price,
        .qty_total      = state.qty_total,
        .qty_filled     = state.qty_filled,
        .message        = state.reject_reason,
        .ts             = state.ts,
    };

    {
        std::unique_lock lock(orders_mu_);
        orders_[local_id] = std::move(state);
    }

    if (on_update_) on_update_(upd);
}

// ── run_order_stream ──────────────────────────────────────────────────────────
//
// SubscribeOrders — стрим входящих обновлений заявок.
// Stream TTL 86400s — штатный reconnect раз в сутки.

void OrderClient::run_order_stream() {
    spdlog::info("[Order] order stream started account={}", account_id_);

    while (!stop_.load(std::memory_order_acquire)) {
        auto ctx = make_context();

        proto_orders::SubscribeOrdersRequest req;
        req.set_account_id(account_id_);

        auto stream = orders_stub_->SubscribeOrders(ctx.get(), req);

        proto_orders::SubscribeOrdersResponse resp;
        while (!stop_.load(std::memory_order_acquire) && stream->Read(&resp)) {
            for (const auto& o : resp.orders()) {
                // Ищем local_id по client_order_id
                int32_t local_id = 0;
                try { local_id = std::stoi(o.order().client_order_id()); }
                catch (...) {}

                // qty_filled из executed_quantity
                int32_t filled = 0;
                try {
                    filled = static_cast<int32_t>(
                        std::stod(o.executed_quantity().value()));
                } catch (...) {}

                upsert(OrderState{
                    .order_id      = o.order_id(),
                    .local_id      = local_id,
                    .symbol        = {},   // symbol из o.order().symbol() если нужен
                    .side          = o.order().side() == ::grpc::tradeapi::v1::SIDE_BUY
                                     ? OrderSide::Buy : OrderSide::Sell,
                    .status        = map_status(o.status()),
                    .type          = o.order().type() == proto_orders::ORDER_TYPE_MARKET
                                     ? OrderType::Market : OrderType::Limit,
                    .price         = 0.0,
                    .qty_total     = static_cast<int32_t>(
                                     std::stod(o.initial_quantity().value())),
                    .qty_filled    = filled,
                    .reject_reason = {},
                    .ts            = std::chrono::system_clock::now(),
                });
            }
        }

        if (stop_.load(std::memory_order_acquire)) break;

        const auto st = stream->Finish();
        spdlog::warn("[Order] stream ended: {} — reconnect in 5s",
            st.error_message());
        std::this_thread::sleep_for(std::chrono::seconds{5});
    }

    spdlog::info("[Order] order stream stopped");
}

} // namespace finam::order
