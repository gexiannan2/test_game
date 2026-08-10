#include <cstdlib>
#include <iostream>
#include <string>

#include "example_service.h"
#include "echo.zrpc.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/rpc/server.h"

namespace {

constexpr uint16_t kPort = 16380;

}  // namespace

int main(int argc, char* argv[]) {
  uint16_t port = kPort;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--port=", 0) == 0) {
      port = static_cast<uint16_t>(std::atoi(arg.substr(7).c_str()));
    }
  }

  rpc_example::EchoRpcHandlerImpl service;
  zrpc::EventLoop loop;
  zrpc::rpc::ServerOptions server_opts;
  server_opts.worker_threads = 4;
  zrpc::rpc::Server server(&loop, "0.0.0.0", port, server_opts);
  echo::RegisterEchoRpc(&server, &service);
  server.Start();
  LOG_INFO << "native rpc server listening on port " << port;
  loop.Run();
  server.PrepareShutdown();
  return 0;
}
