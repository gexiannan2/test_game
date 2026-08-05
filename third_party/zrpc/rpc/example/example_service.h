#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "echo.zrpc.h"
#include "zrpc/rpc/reply.h"

namespace rpc_example {

inline std::atomic<int> g_flaky_remaining{0};

class EchoRpcHandlerImpl : public echo::EchoRpcHandler {
 public:
  zrpc::rpc::Reply Ping(const echo::PingRequest& request,
                        echo::PongRepsonse* response) override {
    if (request.id().empty()) {
      return zrpc::rpc::Reply::Error(zrpc::rpc::ErrorCode::kInvalidRequest,
                                     "empty request");
    }
    response->set_id("pong:" + request.id());
    return zrpc::rpc::Reply::Ok(std::string());
  }

  zrpc::rpc::Reply Fail(const echo::PingRequest& /*request*/,
                        echo::PongRepsonse* /*response*/) override {
    return zrpc::rpc::Reply::Error(zrpc::rpc::ErrorCode::kInvalidRequest,
                                   "forced failure");
  }

  zrpc::rpc::Reply Slow(const echo::PingRequest& request,
                        echo::PongRepsonse* response) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    response->set_id(request.id());
    return zrpc::rpc::Reply::Ok(std::string());
  }

  zrpc::rpc::Reply Flaky(const echo::PingRequest& request,
                         echo::PongRepsonse* response) override {
    if (g_flaky_remaining.fetch_sub(1) > 0) {
      return zrpc::rpc::Reply::Error(zrpc::rpc::ErrorCode::kTransport,
                                     "temporary failure");
    }
    response->set_id("recovered:" + request.id());
    return zrpc::rpc::Reply::Ok(std::string());
  }
};

}  // namespace rpc_example
