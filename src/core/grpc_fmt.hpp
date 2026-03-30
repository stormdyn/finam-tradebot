#pragma once
// Форматтеры для gRPC-типов, несовместимых с fmt out-of-the-box.
// Включать ПЕРЕД <spdlog/spdlog.h> во всех .cpp, которые логируют gRPC-статусы.
#include <fmt/format.h>
#include <grpcpp/support/status_code_enum.h>

template <>
struct fmt::formatter<grpc::StatusCode> : fmt::formatter<int> {
    auto format(grpc::StatusCode c, fmt::format_context& ctx) const {
        return fmt::formatter<int>::format(static_cast<int>(c), ctx);
    }
};
