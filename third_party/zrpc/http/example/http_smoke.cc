#include "zrpc/http/http_server.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void CloseSocket(SocketHandle socket_handle) {
#ifdef _WIN32
  closesocket(socket_handle);
#else
  close(socket_handle);
#endif
}

bool SetSocketTimeout(SocketHandle socket_handle) {
#ifdef _WIN32
  const DWORD timeout_ms = 2000;
  return setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&timeout_ms),
                    sizeof(timeout_ms)) == 0 &&
         setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&timeout_ms),
                    sizeof(timeout_ms)) == 0;
#else
  const timeval timeout = {2, 0};
  return setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                    sizeof(timeout)) == 0 &&
         setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                    sizeof(timeout)) == 0;
#endif
}

bool RunClient(uint16_t port, std::string* response) {
  SocketHandle socket_handle = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_handle == kInvalidSocket) {
    return false;
  }
  if (!SetSocketTimeout(socket_handle)) {
    CloseSocket(socket_handle);
    return false;
  }

  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
      connect(socket_handle, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) != 0) {
    CloseSocket(socket_handle);
    return false;
  }

  const std::string request =
      "GET /smoke HTTP/1.1\r\nHost: localhost\r\n\r\n";
  size_t sent = 0;
  while (sent < request.size()) {
#ifdef _WIN32
    const int written =
        send(socket_handle, request.data() + sent,
             static_cast<int>(request.size() - sent), 0);
#else
    const ssize_t written =
        send(socket_handle, request.data() + sent, request.size() - sent, 0);
#endif
    if (written <= 0) {
      CloseSocket(socket_handle);
      return false;
    }
    sent += static_cast<size_t>(written);
  }

  char buffer[1024];
  while (true) {
#ifdef _WIN32
    const int received = recv(socket_handle, buffer, sizeof(buffer), 0);
#else
    const ssize_t received = recv(socket_handle, buffer, sizeof(buffer), 0);
#endif
    if (received == 0) {
      break;
    }
    if (received < 0) {
      CloseSocket(socket_handle);
      return false;
    }
    response->append(buffer, static_cast<size_t>(received));
  }
  CloseSocket(socket_handle);
  return true;
}

}

int main(int argc, char* argv[]) {
#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return 1;
  }
#endif

  uint16_t port = 18082;
  if (argc > 1) {
    const long parsed = std::strtol(argv[1], nullptr, 10);
    if (parsed <= 0 || parsed > 32767) {
      return 1;
    }
    port = static_cast<uint16_t>(parsed);
  }

  std::promise<zrpc::HttpServer*> ready;
  std::future<zrpc::HttpServer*> ready_future = ready.get_future();
  std::thread server_thread([&ready, port]() {
    zrpc::HttpServer server("127.0.0.1", port);
    server.SetMessageCallback(
        [](const std::shared_ptr<zrpc::TcpConnection>&,
           const zrpc::HttpRequest& request, zrpc::HttpResponse* response) {
          if (request.GetMethod() == zrpc::HttpRequest::kGet &&
              request.GetPath() == "/smoke") {
            response->SetStatusCode(zrpc::HttpResponse::k200k);
            response->SetStatusMessage("OK");
            response->SetContentType("text/plain");
            response->SetBody("smoke-ok");
          } else {
            response->SetStatusCode(zrpc::HttpResponse::k404NotFound);
            response->SetStatusMessage("Not Found");
          }
        });
    server.Start();
    ready.set_value(&server);
    server.Run();
  });

  zrpc::HttpServer* server = nullptr;
  if (ready_future.wait_for(std::chrono::seconds(2)) ==
      std::future_status::ready) {
    server = ready_future.get();
  }

  std::string response;
  const bool request_ok = server != nullptr && RunClient(port, &response);
  if (server != nullptr) {
    server->Stop();
  }
  server_thread.join();

#ifdef _WIN32
  WSACleanup();
#endif

  const bool response_ok =
      response.find("HTTP/1.1 200 OK\r\n") == 0 &&
      response.find("Content-Length: 8\r\n") != std::string::npos &&
      response.find("Connection: close\r\n") != std::string::npos &&
      response.size() >= 8 && response.substr(response.size() - 8) == "smoke-ok";
  if (!request_ok || !response_ok) {
    std::cerr << "HTTP smoke failed" << std::endl;
    return 1;
  }
  std::cout << "HTTP smoke passed" << std::endl;
  return 0;
}
