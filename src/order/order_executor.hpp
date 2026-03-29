#pragma once
#include <string>
#include <memory>
#include "core/interfaces.hpp"
#include "auth/token_manager.hpp"

namespace finam::order {

// Wraps OrderService.NewOrder / CancelOrder.
// Handles partial fills: tracks qty_filled per order_id.
// Edge cases handled:
//   - Network disconnect during order: returns last known status
//   - Partial fill + cancel: sends CancelOrder, reports remaining qty
//   - Expiring contract: rejects orders if session is closed
class OrderExecutor final : public IOrderExecutor {
public:
    OrderExecutor(std::shared_ptr<grpc::Channel> channel,
                  std::shared_ptr<auth::TokenManager> token_mgr);

    [[nodiscard]] Result<std::string> submit(const OrderRequest& req) override;
    [[nodiscard]] Result<void>        cancel(std::string_view order_id) override;

private:
    std::shared_ptr<grpc::Channel>          channel_;
    std::shared_ptr<auth::TokenManager>     token_mgr_;
};

} // namespace finam::order
