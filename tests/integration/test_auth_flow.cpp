#include <catch2/catch_test_macros.hpp>
#include <grpcpp/grpcpp.h>
#include <thread>
#include <atomic>

#include "grpc/tradeapi/v1/auth/auth_service.grpc.pb.h"
#include "auth/token_manager.hpp"

namespace proto_auth = ::grpc::tradeapi::v1::auth;

// ── MockAuthServiceImpl ───────────────────────────────────────────────────────

class MockAuthServiceImpl final
    : public proto_auth::AuthService::Service
{
public:
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

    grpc::Status SubscribeJwtRenewal(
        grpc::ServerContext* ctx,
        const proto_auth::SubscribeJwtRenewalRequest*,
        grpc::ServerWriter<proto_auth::SubscribeJwtRenewalResponse>* writer) override
    {
        proto_auth::SubscribeJwtRenewalResponse ev;
        ev.set_token("renewed_jwt");
        writer->Write(ev);
        // Wait for client cancellation — but with a hard timeout to avoid
        // hanging forever if the client disconnects without cancelling.
        for (int i = 0; i < 100 && !ctx->IsCancelled(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        return grpc::Status::OK;
    }
};

// ── Fixture ───────────────────────────────────────────────────────────────────

struct GrpcFixture {
    MockAuthServiceImpl   service;
    std::unique_ptr<grpc::Server> server;
    std::string           addr;

    GrpcFixture() {
        int port = 0;
        grpc::ServerBuilder builder;
        builder.AddListeningPort("localhost:0",
            grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        addr   = "localhost:" + std::to_string(port);
    }

    ~GrpcFixture() {
        // Immediate shutdown — unblocks any streaming RPCs
        server->Shutdown();
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("Auth flow: valid secret obtains JWT and account_id",
          "[integration][auth]")
{
    GrpcFixture fix;

    finam::auth::TokenManager mgr{
        finam::auth::TokenManagerConfig{
            .endpoint = fix.addr,
            .use_tls  = false,
        }
    };

    const auto r = mgr.init("valid_secret");
    // Shutdown before fixture destructor kills the server —
    // stops the renewal thread cleanly while channel is still alive.
    mgr.shutdown();

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
    mgr.shutdown();

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

    // Renewal thread already running inside init().
    // Mock sends the event immediately on connect.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    CHECK(mgr.jwt() == "renewed_jwt");

    // Shutdown BEFORE fixture destructor kills the server.
    // This cancels the gRPC stream cleanly so renewal_thread_ can join.
    mgr.shutdown();
}
