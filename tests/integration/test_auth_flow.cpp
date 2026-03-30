#include <catch2/catch_test_macros.hpp>
#include <grpcpp/grpcpp.h>
#include <grpcpp/test/mock_stream.h>
#include <thread>
#include <atomic>

// ── Mock AuthService ────────────────────────────────────────────────────────────────
//
// Запускаем встроенный gRPC-сервер в памяти (без сети).
// TokenManager подключаемся к нему через localhost:порт.
//
// Трейдофф: используем grpc::ServerBuilder, а не GoogleMock-стабы,
// чтобы не зависеть от gmock. Тест верифицирует весь auth-флоу.

#include "grpc/tradeapi/v1/auth/auth_service.grpc.pb.h"
#include "auth/token_manager.hpp"

namespace proto_auth = ::grpc::tradeapi::v1::auth;

// ── MockAuthService (in-process gRPC impl) ────────────────────────────────────

class MockAuthServiceImpl final
    : public proto_auth::AuthService::Service
{
public:
    // Auth(secret) → возвращаем jwt="test_jwt"
    grpc::Status Auth(
        grpc::ServerContext*,
        const proto_auth::AuthRequest* req,
        proto_auth::AuthResponse* resp) override
    {
        if (req->secret_token() == "valid_secret") {
            resp->set_jwt("test_jwt");
            return grpc::Status::OK;
        }
        return grpc::Status{grpc::StatusCode::UNAUTHENTICATED, "bad secret"};
    }

    // TokenDetails(jwt) → account_ids=["ACC001"]
    grpc::Status TokenDetails(
        grpc::ServerContext*,
        const proto_auth::TokenDetailsRequest* req,
        proto_auth::TokenDetailsResponse* resp) override
    {
        if (req->jwt() == "test_jwt") {
            resp->add_account_ids("ACC001");
            return grpc::Status::OK;
        }
        return grpc::Status{grpc::StatusCode::UNAUTHENTICATED, "bad jwt"};
    }

    // SubscribeJwtRenewal — шлём один renewal и закрываем поток
    grpc::Status SubscribeJwtRenewal(
        grpc::ServerContext* ctx,
        const proto_auth::SubscribeJwtRenewalRequest*,
        grpc::ServerWriter<proto_auth::JwtRenewalEvent>* writer) override
    {
        proto_auth::JwtRenewalEvent ev;
        ev.set_jwt("renewed_jwt");
        writer->Write(ev);
        // Ждём отмены от клиента
        while (!ctx->IsCancelled())
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return grpc::Status::OK;
    }
};

// ── Фикстура: запуск/остановка сервера ─────────────────────────────────

struct GrpcFixture {
    MockAuthServiceImpl  service;
    std::unique_ptr<grpc::Server> server;
    std::string          addr;

    GrpcFixture() {
        // Адрес 0 — OS выберет свободный порт
        int port = 0;
        grpc::ServerBuilder builder;
        builder.AddListeningPort("localhost:0",
            grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        addr   = "localhost:" + std::to_string(port);
    }
    ~GrpcFixture() {
        server->Shutdown(std::chrono::system_clock::now()
            + std::chrono::milliseconds{100});
    }
};

// ── Тесты ────────────────────────────────────────────────────────────────────────

TEST_CASE("Auth flow: valid secret obtains JWT and account_id",
          "[integration][auth]")
{
    GrpcFixture fix;

    finam::auth::TokenManager mgr{
        finam::auth::TokenManagerConfig{
            .endpoint = fix.addr,
            .use_tls  = false,  // insecure — in-process mock
        }
    };

    const auto r = mgr.init("valid_secret");
    REQUIRE(r.has_value());
    CHECK(mgr.jwt()                == "test_jwt");
    CHECK(mgr.primary_account_id() == "ACC001");
}

TEST_CASE("Auth flow: invalid secret returns error",
          "[integration][auth]")
{
    GrpcFixture fix;

    finam::auth::TokenManager mgr{
        finam::auth::TokenManagerConfig{
            .endpoint = fix.addr,
            .use_tls  = false,
        }
    };

    const auto r = mgr.init("wrong_secret");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == finam::ErrorCode::RpcError);
}

TEST_CASE("JWT renewal: SubscribeJwtRenewal emits renewed_jwt",
          "[integration][auth]")
{
    GrpcFixture fix;

    finam::auth::TokenManager mgr{
        finam::auth::TokenManagerConfig{
            .endpoint = fix.addr,
            .use_tls  = false,
        }
    };
    REQUIRE(mgr.init("valid_secret").has_value());

    // Запускаем реньюал-поток, ждём первого реньюала
    mgr.start_jwt_renewal();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    CHECK(mgr.jwt() == "renewed_jwt");
    mgr.shutdown();
}
