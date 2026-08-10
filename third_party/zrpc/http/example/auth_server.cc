#include <iostream>
#include <memory>
#include <string>

#include "zrpc/base/logger.h"
#include "zrpc/http/auth_middleware.h"
#include "zrpc/http/http_server.h"
#include "zrpc/http/player_auth.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {

constexpr char kJwtSecret[] =
    "zrpc-demo-jwt-secret-change-me-32-bytes-minimum";

std::string JsonEscape(const std::string& input) {
  std::string out;
  for (char ch : input) {
    if (ch == '"') {
      out += "\\\"";
    } else if (ch == '\\') {
      out += "\\\\";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string ParseFormField(const std::string& body, const std::string& key) {
  const std::string prefix = key + "=";
  const size_t pos = body.find(prefix);
  if (pos == std::string::npos) {
    return {};
  }
  size_t end = body.find('&', pos);
  if (end == std::string::npos) {
    end = body.size();
  }
  return body.substr(pos + prefix.size(), end - pos - prefix.size());
}

void WriteJson(zrpc::HttpResponse* response, int code, const std::string& /*message*/,
               const std::string& body) {
  response->SetStatusCode(static_cast<zrpc::HttpResponse::HttpStatusCode>(code));
  response->SetStatusMessage(code == 200 ? "OK" : "Error");
  response->SetContentType("application/json");
  response->SetBody(body);
}

void BootstrapDemoAccounts(zrpc::http::PlayerAuthService* auth) {
  auto hasher = zrpc::http::NewPbkdf2Hasher();
  if (hasher == nullptr) {
  return;
  }

  auto add_user = [&](const std::string& player_id, const std::string& username,
                      const std::string& password, const std::string& role) {
    std::string encoded;
    if (!hasher->Hash(password, &encoded)) {
      return;
    }
    zrpc::http::PlayerAccount account;
    account.player_id = player_id;
    account.username = username;
    account.password_hash = encoded;
    account.role = role;
    auth->AddAccount(account);
  };

  add_user("10001", "alice", "alice123", "player");
  add_user("10002", "bob", "bob123", "player");
  add_user("90001", "admin", "admin123", "admin");
}

}  // namespace

int main() {
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    LOG_SYSERR << "WSAStartup failed";
    return 1;
  }
#endif

  if (!zrpc::http::CryptoBackendAvailable()) {
    LOG_SYSERR << "OpenSSL backend unavailable, build with -DZRPC_USE_OPENSSL";
    return 1;
  }

  auto signer = zrpc::http::NewHs256Signer(kJwtSecret);
  auto codec = std::make_shared<zrpc::http::JwtCodec>(signer);
  auto hasher = zrpc::http::NewPbkdf2Hasher();
  zrpc::http::PlayerAuthOptions auth_options;
  auth_options.payload_encryption_key =
      "demo-payload-key-change-me-32byt";
  zrpc::http::PlayerAuthService player_auth(
      codec, std::move(hasher), std::move(auth_options));
  BootstrapDemoAccounts(&player_auth);

  auto auth = std::make_shared<zrpc::http::AuthMiddleware>(codec);

  zrpc::HttpServer server("127.0.0.1", 8080);
  server.SetMessageCallback(
      [&](const std::shared_ptr<zrpc::TcpConnection>& conn,
          const zrpc::HttpRequest& request, zrpc::HttpResponse* response) {
        const std::string& path = request.GetPath();

        if (request.GetMethod() == zrpc::HttpRequest::kPost && path == "/login") {
          const std::string username = ParseFormField(request.GetBody(), "username");
          const std::string password = ParseFormField(request.GetBody(), "password");

          std::string token;
          zrpc::http::PlayerSession session;
          const zrpc::http::CryptoStatus status =
              player_auth.Login(username, password, &token, &session);
          if (!status) {
            WriteJson(response, 401, "login failed",
                      "{\"error\":\"invalid username or password\"}");
            return;
          }

          WriteJson(response, 200, "ok",
                    "{\"access_token\":\"" + JsonEscape(token) +
                        "\",\"token_type\":\"Bearer\",\"player_id\":\"" +
                        JsonEscape(session.player_id) + "\",\"username\":\"" +
                        JsonEscape(session.username) + "\",\"role\":\"" +
                        JsonEscape(session.role) + "\"}");
          return;
        }

        if (request.GetMethod() == zrpc::HttpRequest::kGet && path == "/profile") {
          zrpc::http::JwtClaims claims;
          if (!auth->Authorize(request, &claims, response)) {
            return;
          }
          WriteJson(response, 200, "ok",
                    "{\"player_id\":\"" + JsonEscape(claims.Get("player_id")) +
                        "\",\"username\":\"" + JsonEscape(claims.Get("username")) +
                        "\",\"role\":\"" + JsonEscape(claims.Get("role")) + "\"}");
          return;
        }

        if (request.GetMethod() == zrpc::HttpRequest::kPost &&
            path == "/secure-payload") {
          zrpc::http::JwtClaims claims;
          if (!auth->Authorize(request, &claims, response)) {
            return;
          }

          std::string encrypted;
          const zrpc::http::CryptoStatus enc =
              player_auth.EncryptPlayerPayload(request.GetBody(), &encrypted);
          if (!enc) {
            WriteJson(response, 500, "encrypt failed",
                      "{\"error\":\"encrypt failed\"}");
            return;
          }

          std::string decrypted;
          const zrpc::http::CryptoStatus dec =
              player_auth.DecryptPlayerPayload(encrypted, &decrypted);
          if (!dec) {
            WriteJson(response, 500, "decrypt failed",
                      "{\"error\":\"decrypt failed\"}");
            return;
          }

          WriteJson(response, 200, "ok",
                    "{\"player_id\":\"" + JsonEscape(claims.Get("player_id")) +
                        "\",\"encrypted\":\"" + JsonEscape(encrypted) +
                        "\",\"roundtrip\":\"" + JsonEscape(decrypted) + "\"}");
          return;
        }

        if (path == "/") {
          response->SetStatusCode(zrpc::HttpResponse::k200k);
          response->SetStatusMessage("OK");
          response->SetContentType("text/plain");
          response->SetBody(
              "zrpc http auth demo\n"
              "POST /login username=alice&password=alice123\n"
              "GET /profile Authorization: Bearer <token>\n"
              "POST /secure-payload Authorization: Bearer <token>\n");
          return;
        }

        response->SetStatusCode(zrpc::HttpResponse::k404NotFound);
        response->SetStatusMessage("Not Found");
      });

  LOG_INFO << "auth_server listening on 127.0.0.1:8080";
  if (!server.Start()) {
    LOG_SYSERR << "auth_server failed to start";
    return 1;
  }
  server.Run();

#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
