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
    int64_t     local_id{};   // FIX: int64_t (Finam order IDs are 64-bit)
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
    explicit OrderClient(
        std::shared_ptr<auth::TokenManager> token_mgr,
        std::string                         account_id,
        OrderUpdateCallback                 on_update = {}
    );
    ~OrderClient() override;

    OrderClient(const OrderClient&)            = delete;
    OrderClient& operator=(const OrderClient&) = delete;

    // ── IOrderExecutor ────────────────────────────────────────────────────────
    // FIX: Result<int64_t> — соответствует IOrderExecutor и Finam API
    [[nodiscard]] Result<int64_t> submit(const OrderRequest& req) override;
    [[nodiscard]] Result<void>    cancel(int64_t order_no,
                                         std::string_view client_id) override;

    // ── Query ─────────────────────────────────────────────────────────────────
    [[nodiscard]] std::vector<OrderState>   active_orders() const;
    [[nodiscard]] std::optional<OrderState> find(int64_t local_id) const; // FIX: int64_t

    // ── Callback ──────────────────────────────────────────────────────────────
    void set_update_callback(OrderUpdateCallback cb) noexcept {
        std::lock_guard lock(cb_mu_);
        on_update_ = std::move(cb);
    }

    void shutdown() noexcept;

private:
    [[nodiscard]] std::unique_ptr<grpc::ClientContext> make_context() const;
    [[nodiscard]] int64_t next_id() noexcept; // FIX: int64_t

    void upsert(OrderState state);
    void run_order_stream();

    void invoke_callback(const OrderUpdate& upd) {
        std::lock_guard lock(cb_mu_);
        if (on_update_) on_update_(upd);
    }

    std::shared_ptr<auth::TokenManager> token_mgr_;
    std::string                         account_id_;

    mutable std::mutex  cb_mu_;
    OrderUpdateCallback on_update_;

    std::unique_ptr<::grpc::tradeapi::v1::orders::OrdersService::Stub> orders_stub_;

    std::atomic<int64_t>                    id_counter_{1};  // FIX: int64_t
    mutable std::shared_mutex               orders_mu_;
    std::unordered_map<int64_t, OrderState> orders_;         // FIX: int64_t key

    std::atomic<bool> stop_{false};
    std::thread       stream_thread_;
};

} // namespace finam::order
