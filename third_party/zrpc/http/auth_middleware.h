#pragma once

#include <functional>
#include <memory>
#include <string>

#include "zrpc/http/http_request.h"
#include "zrpc/http/http_response.h"
#include "zrpc/http/jwt.h"
#include "zrpc/net/tcp_connection.h"

namespace zrpc {
namespace http {

class AuthMiddleware {
 public:
  explicit AuthMiddleware(std::shared_ptr<JwtCodec> codec);

  bool Authorize(const HttpRequest& request, JwtClaims* claims,
                 HttpResponse* response) const;

  static std::string ExtractBearerToken(const HttpRequest& request);
  static void WriteUnauthorized(HttpResponse* response, const std::string& message);
  static void WriteForbidden(HttpResponse* response, const std::string& message);

 private:
  std::shared_ptr<JwtCodec> codec_;
};

using ProtectedHandler = std::function<void(
    const std::shared_ptr<TcpConnection>& conn, const HttpRequest& request,
    HttpResponse* response, const JwtClaims& claims)>;

class ProtectedRoute {
 public:
  ProtectedRoute(std::shared_ptr<AuthMiddleware> auth, ProtectedHandler handler);

  void Handle(const std::shared_ptr<TcpConnection>& conn,
              const HttpRequest& request, HttpResponse* response) const;

 private:
  std::shared_ptr<AuthMiddleware> auth_;
  ProtectedHandler handler_;
};

}  // namespace http
}  // namespace zrpc
