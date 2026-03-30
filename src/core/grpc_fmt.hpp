#pragma once
// Форматтеры для gRPC-типов для spdlog bundled fmt.
// Включать ПЕРЕД <spdlog/spdlog.h> во всех .cpp где логируются gRPC-статусы.
#include <spdlog/fmt/fmt.h>
#include <grpcpp/support/status_code_enum.h>

template <>
struct fmt::formatter<grpc::StatusCode> : fmt::formatter<int> {
    auto format(grpc::StatusCode c, fmt::format_context& ctx) const {
        return fmt::formatter<int>::format(static_cast<int>(c), ctx);
    }
};
