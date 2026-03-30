#include "order_client.hpp"
#include <spdlog/spdlog.h>

// TODO: после подключения proto раскомментировать:
// #include "grpc/tradeapi/v2/orders/order_service.grpc.pb.h"
// #include "grpc/tradeapi/v2/accounts/account_service.grpc.pb.h"

namespace finam::order {

// ── Helpers ───────────────────────────────────────────────────────────────────

namespace {

// Валидация запроса до отправки в API
std::optional<Error> validate(const OrderRequest& req) {
    if (req.client_id.empty())
        return Error{ErrorCode::InvalidArgument, "client_id is empty"};
    if (req.quantity <= 0)
        return Error{ErrorCode::InvalidQuantity,
            "quantity must be > 0, got " + std::to_string(req.quantity)};
    if (req.type == OrderType::Limit && req.price <= 0.0)
        return Error{ErrorCode::InvalidPrice,
            "limit order requires price > 0"};
    return std::nullopt;
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
{
    // Запускаем фоновый стрим ордеров
    stream_thread_ = std::thread(&OrderClient::run_order_stream, this);
    spdlog::debug("[Order] OrderClient created, account={}", account_id_);
}

OrderClient::~OrderClient() { shutdown(); }

void OrderClient::shutdown() noexcept {
    if (stop_.exchange(true, std::memory_order_acq_rel))
        return;  // уже останавливается — выходим
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

    // TODO: реальный gRPC вызов после подключения proto:
    //
    // auto ctx = make_context();
    // tradeapi::v2::NewOrderRequest grpc_req;
    // grpc_req.set_account_id(account_id_);
    // grpc_req.set_symbol(sym);
    // grpc_req.mutable_quantity()->set_value(std::to_string(req.quantity));
    // grpc_req.set_side(req.side == OrderSide::Buy
    //     ? tradeapi::v2::SIDE_BUY : tradeapi::v2::SIDE_SELL);
    //
    // if (req.type == OrderType::Market) {
    //     grpc_req.set_type(tradeapi::v2::ORDER_TYPE_MARKET);
    //     grpc_req.set_time_in_force(tradeapi::v2::TIME_IN_FORCE_FILL_OR_KILL);
    // } else {
    //     grpc_req.set_type(tradeapi::v2::ORDER_TYPE_LIMIT);
    //     grpc_req.mutable_limit_price()->set_value(std::to_string(req.price));
    //     grpc_req.set_time_in_force(tradeapi::v2::TIME_IN_FORCE_DAY);
    // }
    // grpc_req.set_client_order_id(std::to_string(local_id));
    //
    // tradeapi::v2::OrderState resp;
    // auto status = order_stub_->NewOrder(ctx.get(), grpc_req, &resp);
    // if (!status.ok())
    //     return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});
    //
    // upsert(OrderState{
    //     .order_id     = resp.order_id(),
    //     .local_id     = local_id,
    //     .symbol       = req.symbol,
    //     .side         = req.side,
    //     .status       = OrderStatus::Pending,
    //     .type         = req.type,
    //     .price        = req.price,
    //     .qty_total    = req.quantity,
    //     .reject_reason = {},
    //     .ts           = std::chrono::system_clock::now(),
    // });

    // Stub: сохраняем в трекер как Pending
    upsert(OrderState{
        .order_id      = "stub-" + std::to_string(local_id),
        .local_id      = local_id,
        .symbol        = req.symbol,
        .side          = req.side,
        .status        = OrderStatus::Pending,
        .type          = req.type,
        .price         = req.price,
        .qty_total     = req.quantity,
        .reject_reason = {},
        .ts            = std::chrono::system_clock::now(),
    });

    return local_id;
}

// ── cancel ────────────────────────────────────────────────────────────────────

Result<void> OrderClient::cancel(int64_t order_no,
                                  std::string_view client_id) {
    spdlog::info("[Order] cancel order_no={} client_id={}", order_no, client_id);

    // TODO: реальный gRPC вызов:
    //
    // auto ctx = make_context();
    // tradeapi::v2::CancelOrderRequest req;
    // req.set_account_id(std::string(client_id));
    // req.set_order_id(std::to_string(order_no));
    // tradeapi::v2::CancelOrderResponse resp;
    // auto status = order_stub_->CancelOrder(ctx.get(), req, &resp);
    // if (!status.ok())
    //     return std::unexpected(Error{ErrorCode::RpcError, status.error_message()});

    // Stub: помечаем как Cancelled в трекере
    {
        std::unique_lock lock(orders_mu_);
        for (auto& [id, state] : orders_) {
            if (state.status == OrderStatus::Pending ||
                state.status == OrderStatus::PartialFill)
            {
                state.status = OrderStatus::Cancelled;
                spdlog::debug("[Order] stub cancel local_id={}", id);
            }
        }
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

    // Конвертируем в OrderUpdate для колбэка
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

// ── run_order_stream (stub) ───────────────────────────────────────────────────

void OrderClient::run_order_stream() {
    spdlog::info("[Order] order stream started account={}", account_id_);

    // TODO: реальный SubscribeOrderTrades стрим:
    //
    // while (!stop_.load()) {
    //     auto ctx = make_context();
    //     tradeapi::v2::SubscribeOrderTradesRequest req;
    //     req.set_account_id(account_id_);
    //     req.set_action(tradeapi::v2::ACTION_SUBSCRIBE);
    //     req.set_data_type(tradeapi::v2::DATA_TYPE_ALL);
    //     auto stream = acc_stub_->SubscribeOrderTrades(ctx.get(), req);
    //
    //     tradeapi::v2::SubscribeOrderTradesResponse resp;
    //     while (!stop_.load() && stream->Read(&resp)) {
    //         for (const auto& o : resp.orders())
    //             upsert(order_state_from_proto(o));
    //     }
    //     // Stream TTL 86400s или разрыв — reconnect
    //     if (!stop_.load()) {
    //         spdlog::warn("[Order] stream disconnected, reconnecting in 5s");
    //         std::this_thread::sleep_for(std::chrono::seconds{5});
    //     }
    // }

    while (!stop_.load())
        std::this_thread::sleep_for(std::chrono::milliseconds{200});

    spdlog::info("[Order] order stream stopped");
}

} // namespace finam::order
