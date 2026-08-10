#include <charconv>
#include <iostream>
#include <limits>
#include <string>

#include "example_service.h"
#include "zrpc/base/logger.h"
#include "zrpc/grpc/rpc_server.h"

namespace {

constexpr uint16_t kDefaultPort = 6379;

uint16_t ParsePort(const char* text) {
  if (text == nullptr) {
    return kDefaultPort;
  }
  const std::string value(text);
  uint32_t port = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), port);
  if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() ||
      port == 0 || port > std::numeric_limits<uint16_t>::max()) {
    return kDefaultPort;
  }
  return static_cast<uint16_t>(port);
}

}  // namespace

int main(int argc, char* argv[]) {
  rpc_example::ServiceMode mode = rpc_example::ServiceMode::kNormal;
  uint16_t port = kDefaultPort;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--mode=", 0) == 0) {
      mode = rpc_example::ParseServiceMode(arg.substr(7));
    } else if (arg.rfind("--port=", 0) == 0) {
      port = ParsePort(arg.substr(7).c_str());
    }
  }

  rpc_example::SudokuServiceImpl service(mode);
  zrpc::EventLoop loop;
  zrpc::RpcServer server(&loop, "127.0.0.1", port);
  server.RegisterService(&service);
  server.Start();

  LOG_INFO << "rpc_server listening on 127.0.0.1:" << port
           << " mode=" << rpc_example::ServiceModeName(mode);

  loop.Run();

  server.PrepareShutdown();
  for (int i = 0; i < 100; ++i) {
    loop.PollOnce(10);
  }
  LOG_INFO << "rpc_server shutdown metrics=" << server.MetricsString()
           << " handled=" << service.handled();
  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}
