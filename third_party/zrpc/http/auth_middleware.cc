#include "zrpc/http/auth_middleware.h"

#include <cctype>
#include <cstring>

namespace zrpc {
namespace http {
namespace {

std::string JsonEscape(const std::string& input) {
  static const char kHex[] = "0123456789abcdef";
  std::string output;
  output.reserve(input.size() + 8);
  for (unsigned char ch : input) {
    switch (ch) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (ch < 0x20) {
          output += "\\u00";
          output.push_back(kHex[(ch >> 4) & 0x0F]);
          output.push_back(kHex[ch & 0x0F]);
        } else {
          output.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return output;
}

bool EqualsIgnoreCase(const std::string& value, size_t begin, size_t end,
                      const char* expected) {
  const size_t expected_size = strlen(expected);
  if (end - begin != expected_size) {
    return false;
  }
  for (size_t i = 0; i < expected_size; ++i) {
    if (std::tolower(static_cast<unsigned char>(value[begin + i])) !=
        std::tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  return true;
}

}

AuthMiddleware::AuthMiddleware(std::shared_ptr<JwtCodec> codec)
    : codec_(std::move(codec)) {}

std::string AuthMiddleware::ExtractBearerToken(const HttpRequest& request) {
  const std::string authorization = request.GetHeader("Authorization");
  size_t pos = 0;
  while (pos < authorization.size() &&
         std::isspace(static_cast<unsigned char>(authorization[pos]))) {
    ++pos;
  }
  const size_t scheme_begin = pos;
  while (pos < authorization.size() &&
         !std::isspace(static_cast<unsigned char>(authorization[pos]))) {
    ++pos;
  }
  if (!EqualsIgnoreCase(authorization, scheme_begin, pos, "Bearer")) {
    return {};
  }
  if (pos == authorization.size()) {
    return {};
  }
  while (pos < authorization.size() &&
         std::isspace(static_cast<unsigned char>(authorization[pos]))) {
    ++pos;
  }
  size_t end = authorization.size();
  while (end > pos &&
         std::isspace(static_cast<unsigned char>(authorization[end - 1]))) {
    --end;
  }
  if (pos == end) {
    return {};
  }
  for (size_t i = pos; i < end; ++i) {
    if (std::isspace(static_cast<unsigned char>(authorization[i]))) {
      return {};
    }
  }
  return authorization.substr(pos, end - pos);
}

void AuthMiddleware::WriteUnauthorized(HttpResponse* response,
                                       const std::string& message) {
  if (response == nullptr) {
    return;
  }
  response->SetStatusCode(HttpResponse::k401Unauthorized);
  response->SetStatusMessage("Unauthorized");
  response->SetContentType("application/json");
  response->SetBody("{\"error\":\"" + JsonEscape(message) + "\"}");
}

void AuthMiddleware::WriteForbidden(HttpResponse* response,
                                    const std::string& message) {
  if (response == nullptr) {
    return;
  }
  response->SetStatusCode(HttpResponse::k403Forbidden);
  response->SetStatusMessage("Forbidden");
  response->SetContentType("application/json");
  response->SetBody("{\"error\":\"" + JsonEscape(message) + "\"}");
}

bool AuthMiddleware::Authorize(const HttpRequest& request, JwtClaims* claims,
                               HttpResponse* response) const {
  if (codec_ == nullptr || claims == nullptr) {
    WriteUnauthorized(response, "auth backend unavailable");
    return false;
  }

  const std::string token = ExtractBearerToken(request);
  if (token.empty()) {
    WriteUnauthorized(response, "missing bearer token");
    return false;
  }

  try {
    const CryptoStatus status = codec_->Decode(token, claims);
    if (!status) {
      WriteUnauthorized(response, "invalid bearer token");
      return false;
    }
  } catch (...) {
    WriteUnauthorized(response, "invalid bearer token");
    return false;
  }
  return true;
}

ProtectedRoute::ProtectedRoute(std::shared_ptr<AuthMiddleware> auth,
                               ProtectedHandler handler)
    : auth_(std::move(auth)), handler_(std::move(handler)) {}

void ProtectedRoute::Handle(const std::shared_ptr<TcpConnection>& conn,
                            const HttpRequest& request,
                            HttpResponse* response) const {
  JwtClaims claims;
  if (auth_ == nullptr) {
    AuthMiddleware::WriteUnauthorized(response, "auth backend unavailable");
    return;
  }
  if (!auth_->Authorize(request, &claims, response)) {
    return;
  }
  if (handler_) {
    handler_(conn, request, response, claims);
  }
}

}  // namespace http
}  // namespace zrpc
