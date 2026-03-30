#pragma once
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/interfaces.hpp"
#include "auth/token_manager.hpp"
#include "grpc/tradeapi/v1/orders/orders_service.grpc.pb.h"

namespace finam::order {

struct OrderState {
    std::string order_id;
    int32_t     local_id{};
    Symbol      symbol;
    OrderSide   side{};
    OrderStatus status{OrderStatus::Pending};
    OrderType   type{};
    double      price{};
    int32_t     qty_total{};
    int32_t     qty_filled{};
    std::string reject_reason;
    Timestamp   ts;
};

using OrderUpdateCallback = std::function<void(const OrderUpdate&)>;

class OrderClient : public IOrderExecutor {
public:
    // Конструктор без callback — его можно установить позже через set_update_callback().
    // Типичный use-case: main создаёт OrderClient, затем StrategyRunner,
    // затем регистрирует callback который знает про runner.
    explicit OrderClient(
        std::shared_ptr<auth::TokenManager> token_mgr,
        std::string                         account_id,
        OrderUpdateCallback                 on_update = {}
    );
    ~OrderClient() override;

    OrderClient(const OrderClient&)            = delete;
    OrderClient& operator=(const OrderClient&) = delete;

    // ── IOrderExecutor ────────────────────────────────────────────────────────────────
    [[nodiscard]] Result<int32_t> submit(const OrderRequest& req) override;
    [[nodiscard]] Result<void>    cancel(int64_t order_no,
                                         std::string_view client_id) override;

    // ── Query ─────────────────────────────────────────────────────────────────────
    [[nodiscard]] std::vector<OrderState>   active_orders() const;
    [[nodiscard]] std::optional<OrderState> find(int32_t local_id) const;

    // ── Callback ─────────────────────────────────────────────────────────────────
    //
    // Заменяет callback атомарно — безопасно вызывать до первого
    // прихода данных из order stream. Stream стартует в конструкторе,
    // но первый ответ от биржи приходит через несколько мс — окно
    // в main достаточно узкое.
    void set_update_callback(OrderUpdateCallback cb) noexcept {
        std::lock_guard lock(cb_mu_);
        on_update_ = std::move(cb);
    }

    void shutdown() noexcept;

private:
    [[nodiscard]] std::unique_ptr<grpc::ClientContext> make_context() const;
    [[nodiscard]] int32_t next_id() noexcept;

    void upsert(OrderState state);
    void run_order_stream();

    // Инвокация on_update_ через обёртку — защищает замену каллбэка
    // посреди его вызова.
    void invoke_callback(const OrderUpdate& upd) {
        std::lock_guard lock(cb_mu_);
        if (on_update_) on_update_(upd);
    }

    std::shared_ptr<auth::TokenManager> token_mgr_;
    std::string                         account_id_;

    mutable std::mutex  cb_mu_;       // защищает on_update_
    OrderUpdateCallback on_update_;   // защищён cb_mu_

    std::unique_ptr<::grpc::tradeapi::v1::orders::OrdersService::Stub> orders_stub_;

    std::atomic<int32_t>                    id_counter_{1};
    mutable std::shared_mutex               orders_mu_;
    std::unordered_map<int32_t, OrderState> orders_;

    std::atomic<bool> stop_{false};
    std::thread       stream_thread_;
};

} // namespace finam::order
