#pragma once
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "core/interfaces.hpp"
#include "auth/token_manager.hpp"

namespace finam::order {

// Активный ордер — внутреннее состояние трекера
struct OrderState {
    std::string order_id;      // биржевой ID (появляется после подтверждения)
    int32_t     local_id{};    // наш счётчик (transaction_id)
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

// Колбэк на обновление ордера — вызывается из фонового потока
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

    // IOrderExecutor — возвращает local_id
    [[nodiscard]] Result<int32_t> submit(const OrderRequest& req) override;
    [[nodiscard]] Result<void>    cancel(int64_t order_no,
                                         std::string_view client_id) override;

    // Снимок активных ордеров (thread-safe)
    [[nodiscard]] std::vector<OrderState> active_orders() const;

    // Поиск по local_id
    [[nodiscard]] std::optional<OrderState> find(int32_t local_id) const;

    void shutdown() noexcept;

private:
    [[nodiscard]] std::unique_ptr<grpc::ClientContext> make_context() const;
    [[nodiscard]] int32_t next_id() noexcept;

    void upsert(OrderState state);   // обновить трекер + вызвать колбэк
    void run_order_stream();         // фоновый поток SubscribeOrderTrades

    std::shared_ptr<auth::TokenManager> token_mgr_;
    std::string                         account_id_;
    OrderUpdateCallback                 on_update_;

    std::atomic<int32_t>                           id_counter_{1};
    mutable std::shared_mutex                      orders_mu_;
    std::unordered_map<int32_t, OrderState>        orders_;   // local_id → state

    std::atomic<bool> stop_{false};
    std::thread       stream_thread_;
};

} // namespace finam::order