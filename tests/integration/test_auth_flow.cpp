#include <catch2/catch_test_macros.hpp>
#include <grpcpp/grpcpp.h>
#include <thread>
#include <atomic>

// ── Mock AuthService ────────────────────────────────────────────────────────────────
//
// Запускаем встроенный gRPC-сервер в памяти (без сети).
// TokenManager подключается к нему через localhost:port.
//
// Трейдофф: используем grpc::ServerBuilder, а не GoogleMock-стабы,
// чтобы не зависеть от gmock. Тест верифицирует весь auth-флоу.

#include "grpc/tradeapi/v1/auth/auth_service.grpc.pb.h"
#include "auth/token_manager.hpp"

namespace proto_auth = ::grpc::tradeapi::v1::auth;

// ── MockAuthServiceImpl (in-process gRPC impl) ───────────────────────────────

class MockAuthServiceImpl final
    : public proto_auth::AuthService::Service
{
public:
    // Auth(secret) → возвращаем token="test_jwt"
    // Поле запроса: secret (не secret_token)
    grpc::Status Auth(
        grpc::ServerContext*,
        const proto_auth::AuthRequest* req,
        proto_auth::AuthResponse* resp) override
    {
        if (req->secret() == "valid_secret") {
            resp->set_token("test_jwt");
            return grpc::Status::OK;
        }
        return grpc::Status{grpc::StatusCode::UNAUTHENTICATED, "bad secret"};
    }

    // TokenDetails(token) → account_ids=["ACC001"]
    // Поле запроса: token (не jwt)
    grpc::Status TokenDetails(
        grpc::ServerContext*,
        const proto_auth::TokenDetailsRequest* req,
        proto_auth::TokenDetailsResponse* resp) override
    {
        if (req->token() == "test_jwt") {
            resp->add_account_ids("ACC001");
            return grpc::Status::OK;
        }
        return grpc::Status{grpc::StatusCode::UNAUTHENTICATED, "bad jwt"};
    }

    // SubscribeJwtRenewal — шлём один renewal и ждём отмены клиента
    // Тип ответа: SubscribeJwtRenewalResponse (не JwtRenewalEvent)
    // Поле ответа: token (не jwt)
    grpc::Status SubscribeJwtRenewal(
        grpc::ServerContext* ctx,
        const proto_auth::SubscribeJwtRenewalRequest*,
        grpc::ServerWriter<proto_auth::SubscribeJwtRenewalResponse>* writer) override
    {
        proto_auth::SubscribeJwtRenewalResponse ev;
        ev.set_token("renewed_jwt");
        writer->Write(ev);
        // Ждём отмены от клиента
        while (!ctx->IsCancelled())
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return grpc::Status::OK;
    }
};

// ── Фикстура: запуск/остановка сервера ──────────────────────────────────────

struct GrpcFixture {
    MockAuthServiceImpl  service;
    std::unique_ptr<grpc::Server> server;
    std::string          addr;

    GrpcFixture() {
        // port=0 — OS выбирает свободный порт
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

// ── Тесты ──────────────────────────────────────────────────────────────────────

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

    // Фоновый renewal-поток уже запущен внутри init().
    // Ждём первого события (mock шлёт сразу после подключения).
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    CHECK(mgr.jwt() == "renewed_jwt");

    mgr.shutdown();
}
