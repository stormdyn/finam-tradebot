#pragma once
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

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
    explicit OrderClient(
        std::shared_ptr<auth::TokenManager> token_mgr,
        std::string                         account_id,
        OrderUpdateCallback                 on_update = {}
    );
    ~OrderClient() override;

    OrderClient(const OrderClient&)            = delete;
    OrderClient& operator=(const OrderClient&) = delete;

    [[nodiscard]] Result<int32_t> submit(const OrderRequest& req) override;
    [[nodiscard]] Result<void>    cancel(int64_t order_no,
                                         std::string_view client_id) override;

    [[nodiscard]] std::vector<OrderState>       active_orders() const;
    [[nodiscard]] std::optional<OrderState>     find(int32_t local_id) const;

    void shutdown() noexcept;

private:
    [[nodiscard]] std::unique_ptr<grpc::ClientContext> make_context() const;
    [[nodiscard]] int32_t next_id() noexcept;

    void upsert(OrderState state);
    void run_order_stream();

    std::shared_ptr<auth::TokenManager> token_mgr_;
    std::string                         account_id_;
    OrderUpdateCallback                 on_update_;

    // gRPC stub — создаётся один раз в конструкторе
    std::unique_ptr<::grpc::tradeapi::v1::orders::OrdersService::Stub> orders_stub_;

    std::atomic<int32_t>                      id_counter_{1};
    mutable std::shared_mutex                 orders_mu_;
    std::unordered_map<int32_t, OrderState>   orders_;

    std::atomic<bool> stop_{false};
    std::thread       stream_thread_;
};

} // namespace finam::order
