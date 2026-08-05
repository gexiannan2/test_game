#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "zrpc/base/buffer.h"
#include "zrpc/http/auth_middleware.h"
#include "zrpc/http/crypto.h"
#include "zrpc/http/http_client.h"
#include "zrpc/http/http_context.h"
#include "zrpc/http/http_request.h"
#include "zrpc/http/http_response.h"
#include "zrpc/http/jwt.h"
#include "zrpc/http/player_auth.h"
#include "zrpc/net/event_loop.h"

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << message << std::endl;
  }
}

size_t CountSubstring(const std::string& text, const std::string& needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

void TestFragmentedRequest() {
  zrpc::HttpContext context;
  zrpc::Buffer buffer;
  buffer.Append("POST /submit?q=1 HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "content-length: 5\r\n"
                "X-Test: yes\r\n\r\nhe");
  Check(context.ParseRequest(&buffer), "fragmented request first parse");
  Check(!context.GotAll(), "fragmented request remains incomplete");
  buffer.Append("llo");
  Check(context.ParseRequest(&buffer), "fragmented request second parse");
  Check(context.GotAll(), "fragmented request completes");
  Check(context.GetRequest().GetBody() == "hello", "fragmented request body");
  Check(context.GetRequest().GetHeader("X-TEST") == "yes",
        "request header lookup is case insensitive");

  context.Reset();
  buffer.Append("GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n");
  Check(context.ParseRequest(&buffer) && context.GotAll(),
        "request parses after reset");
  Check(context.GetRequest().GetBody().empty(), "reset clears request body");
}

void TestInvalidRequests() {
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    buffer.Append("GET / HTTP/1.1\r\nBroken-Header\r\n\r\n");
    Check(!context.ParseRequest(&buffer), "malformed request header rejected");
  }
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    buffer.Append("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n");
    Check(!context.ParseRequest(&buffer), "negative content length rejected");
  }
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    buffer.Append("POST / HTTP/1.1\r\n"
                  "Content-Length: 99999999999999999999\r\n\r\n");
    Check(!context.ParseRequest(&buffer), "overflow content length rejected");
  }
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    buffer.Append("POST / HTTP/1.1\r\n"
                  "Content-Length: 1\r\n"
                  "content-length: 1\r\n\r\nx");
    Check(!context.ParseRequest(&buffer), "duplicate content length rejected");
  }
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    buffer.Append("POST / HTTP/1.1\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    Check(!context.ParseRequest(&buffer),
          "unsupported chunked request rejected");
  }
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    buffer.Append("GET / HTTP/1.1\r\n\r\n");
    Check(!context.ParseRequest(&buffer),
          "http11 request without host rejected");
  }
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    buffer.Append("GET / HTTP/1.1\r\n"
                  "Host: first.test\r\n"
                  "host: second.test\r\n\r\n");
    Check(!context.ParseRequest(&buffer), "duplicate host rejected");
  }
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    buffer.Append("GET / HTTP/1.1\r\n"
                  "Host: example.test\r\n"
                  "Authorization: Bearer one\r\n"
                  "authorization: Bearer two\r\n\r\n");
    Check(!context.ParseRequest(&buffer),
          "duplicate authorization rejected");
  }

  {
    zrpc::HttpContext response_context;
    zrpc::Buffer response_buffer;
    response_buffer.Append(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "Set-Cookie: first=1\r\n"
        "Set-Cookie: second=2\r\n\r\n");
    Check(response_context.ParseResponse(&response_buffer) &&
              response_context.GotAll() &&
              response_context.GetResponse().CountHeader("Set-Cookie") == 2,
          "repeatable response headers preserved");
  }
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    buffer.Append("POST / HTTP/1.1\r\n"
                  "Host: example.test\r\n"
                  "Content-Length: 1\r\n"
                  "Transfer-Encoding: chunked\r\n\r\nx");
    Check(!context.ParseRequest(&buffer), "te plus cl rejected");
  }
  {
    zrpc::HttpContext context;
    zrpc::Buffer buffer;
    std::string request =
        "GET / HTTP/1.1\r\nHost: example.test\r\nX-Test: safe";
    request.push_back('\0');
    request.append("bad\r\n\r\n");
    buffer.Append(request.data(), request.size());
    Check(!context.ParseRequest(&buffer),
          "nul in header value rejected");
  }
  {
    zrpc::HttpContext::Limits limits;
    limits.max_start_line_bytes = 16;
    zrpc::HttpContext context(limits);
    zrpc::Buffer buffer;
    const std::string line = "GET /this-is-too-long";
    bool ok = true;
    for (char ch : line) {
      buffer.Append(&ch, 1);
      ok = context.ParseRequest(&buffer);
      if (!ok) {
        break;
      }
    }
    Check(!ok, "fragmented oversized request line rejected");
  }
  {
    zrpc::HttpContext::Limits limits;
    limits.max_body_bytes = 4;
    zrpc::HttpContext context(limits);
    zrpc::Buffer buffer;
    buffer.Append("POST / HTTP/1.1\r\n"
                  "Host: example.test\r\n"
                  "Content-Length: 5\r\n\r\nhello");
    Check(!context.ParseRequest(&buffer), "oversized body rejected");
  }
}

void TestFragmentedResponse() {
  zrpc::HttpContext context;
  zrpc::Buffer buffer;
  buffer.Append("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe");
  Check(context.ParseResponse(&buffer), "fragmented response first parse");
  Check(!context.GotAll(), "fragmented response remains incomplete");
  buffer.Append("llo");
  Check(context.ParseResponse(&buffer), "fragmented response second parse");
  Check(context.GotAll(), "fragmented response completes");
  Check(context.GetResponse().GetBody() == "hello", "fragmented response body");

  zrpc::HttpContext invalid;
  zrpc::Buffer invalid_buffer;
  invalid_buffer.Append(
      "HTTP/1.1 200 OK\r\nContent-Length: invalid\r\n\r\n");
  Check(!invalid.ParseResponse(&invalid_buffer),
        "invalid response content length rejected");

  zrpc::HttpContext unframed;
  zrpc::Buffer unframed_buffer;
  unframed_buffer.Append("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbody");
  Check(unframed.ParseResponse(&unframed_buffer) && !unframed.GotAll(),
        "close-delimited response waits for eof");
  Check(unframed.ParseResponseEof(&unframed_buffer) && unframed.GotAll() &&
            unframed.GetResponse().GetBody() == "body",
        "close-delimited response completes at eof");

  zrpc::HttpContext unsafe_unframed;
  zrpc::Buffer unsafe_unframed_buffer;
  unsafe_unframed_buffer.Append("HTTP/1.1 200 OK\r\n\r\nbody");
  Check(!unsafe_unframed.ParseResponse(&unsafe_unframed_buffer),
        "persistent unframed response rejected");

  zrpc::HttpContext chunked;
  zrpc::Buffer chunked_buffer;
  chunked_buffer.Append(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
  Check(!chunked.ParseResponse(&chunked_buffer),
        "unsupported chunked response rejected");

  zrpc::HttpContext interim;
  zrpc::Buffer interim_buffer;
  interim_buffer.Append(
      "HTTP/1.1 100 Continue\r\n\r\n"
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  Check(interim.ParseResponse(&interim_buffer) && interim.GotAll() &&
            interim.GetResponse().GetBody() == "ok",
        "interim response followed by final response");

  zrpc::HttpContext head;
  head.SetResponseToHeadRequest(true);
  zrpc::Buffer head_buffer;
  head_buffer.Append("HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\n");
  Check(head.ParseResponse(&head_buffer) && head.GotAll() &&
            head.GetResponse().GetBody().empty(),
        "head response completes without body bytes");
}

void TestSerialization() {
  zrpc::HttpRequest request;
  request.SetMethod(zrpc::HttpRequest::kPost);
  const std::string path = "/items";
  Check(request.SetPath(path.data(), path.data() + path.size()),
        "request path accepted");
  Check(request.SetQuery("page=2"), "request query accepted");
  request.SetBody("x");
  Check(request.AddHeader("Host", "example.test"),
        "request host accepted");
  Check(request.AddHeader("Content-Length", "999"),
        "request content length accepted");
  zrpc::Buffer request_buffer;
  Check(request.AppendToBuffer(&request_buffer),
        "request serialization succeeds");
  const std::string serialized_request =
      request_buffer.RetrieveAllAsString();
  Check(serialized_request.find("POST /items?page=2 HTTP/1.1\r\n") == 0,
        "request method and target serialization");
  Check(serialized_request.find("Content-Length: 1\r\n") != std::string::npos,
        "request content length serialization");
  Check(serialized_request.find("Content-Length: 999") == std::string::npos,
        "request stale content length removed");
  Check(!request.AddHeader("X-Unsafe", "safe\r\nInjected: yes"),
        "request response splitting rejected");

  zrpc::HttpRequest invalid_head;
  invalid_head.SetMethod(zrpc::HttpRequest::kHead);
  Check(invalid_head.AddHeader("Host", "example.test"),
        "head request host accepted");
  invalid_head.SetBody("forbidden");
  zrpc::Buffer invalid_head_buffer;
  Check(!invalid_head.AppendToBuffer(&invalid_head_buffer) &&
            invalid_head_buffer.ReadableBytes() == 0,
        "head request body rejected atomically");

  zrpc::HttpResponse response;
  Check(response.SetStatusCode(zrpc::HttpResponse::k200k),
        "response status accepted");
  Check(response.SetStatusMessage("OK"), "response reason accepted");
  response.SetCloseConnection(true);
  response.SetBody("hello");
  Check(response.AddHeader("Content-Length", "99"),
        "response content length accepted");
  Check(response.AddHeader("Connection", "Keep-Alive"),
        "response connection accepted");
  zrpc::Buffer response_buffer;
  Check(response.AppendToBuffer(&response_buffer),
        "response serialization succeeds");
  const std::string serialized_response =
      response_buffer.RetrieveAllAsString();
  Check(serialized_response.find("Content-Length: 5\r\n") != std::string::npos,
        "response content length serialization");
  Check(CountSubstring(serialized_response, "Content-Length:") == 1,
        "response has one content length");
  Check(serialized_response.find("Connection: close\r\n") != std::string::npos,
        "response close header serialization");
  Check(!response.AddHeader("X-Unsafe", "safe\r\nInjected: yes"),
        "response response splitting rejected");
  Check(!response.SetStatusCode(700), "invalid response status rejected");
  Check(!response.SetStatusMessage("OK\r\nInjected: yes"),
        "invalid response reason rejected");

  zrpc::HttpResponse no_content;
  Check(no_content.SetStatusCode(204), "204 status accepted");
  Check(no_content.SetStatusMessage("No Content"),
        "204 reason accepted");
  no_content.SetBody("must-not-be-sent");
  zrpc::Buffer no_content_buffer;
  Check(no_content.AppendToBuffer(&no_content_buffer),
        "204 response serialization succeeds");
  const std::string serialized_no_content =
      no_content_buffer.RetrieveAllAsString();
  Check(serialized_no_content.find("Content-Length:") == std::string::npos &&
            serialized_no_content.find("must-not-be-sent") ==
                std::string::npos,
        "204 response suppresses body and content length");

  zrpc::HttpResponse head_response;
  Check(head_response.SetStatusCode(200), "head response status accepted");
  Check(head_response.SetStatusMessage("OK"),
        "head response reason accepted");
  head_response.SetBody("hello");
  head_response.SetSuppressBody(true);
  zrpc::Buffer head_response_buffer;
  Check(head_response.AppendToBuffer(&head_response_buffer),
        "head response serialization succeeds");
  const std::string serialized_head =
      head_response_buffer.RetrieveAllAsString();
  Check(serialized_head.find("Content-Length: 5\r\n") != std::string::npos &&
            serialized_head.find("\r\n\r\nhello") == std::string::npos,
        "head response preserves length and suppresses body");
}

void TestBase64() {
  std::string output;
  Check(zrpc::http::Base64UrlDecode("Zg", &output) && output == "f",
        "unpadded base64url decode");
  Check(zrpc::http::Base64UrlDecode("Zg==", &output) && output == "f",
        "padded base64url decode");
  Check(!zrpc::http::Base64UrlDecode("Z", &output),
        "invalid base64url length rejected");
  Check(!zrpc::http::Base64UrlDecode("Zh", &output),
        "noncanonical base64url bits rejected");
  Check(!zrpc::http::Base64UrlDecode("Zg ", &output),
        "base64url whitespace rejected");
}

void TestJwt() {
  if (!zrpc::http::CryptoBackendAvailable()) {
    return;
  }

  const std::string secret =
      "regression-test-secret-with-at-least-32-bytes";
  auto signer = zrpc::http::NewHs256Signer(secret);
  Check(signer != nullptr, "strong jwt signer created");
  Check(zrpc::http::NewHs256Signer("weak") == nullptr,
        "weak jwt key rejected");
  zrpc::http::JwtCodec codec(signer);
  zrpc::http::JwtClaims input;
  input.sub = "player";
  input.exp = 4600;
  input.Set("line", "a\nb");
  std::string token;
  Check(codec.Encode(input, &token, 1000).ok, "jwt encode");

  zrpc::http::JwtClaims decoded;
  decoded.Set("stale", "value");
  Check(codec.Decode(token, &decoded, 1001).ok, "jwt decode");
  Check(decoded.Get("line") == "a\nb", "jwt escaped string round trip");
  Check(decoded.Get("stale").empty(), "jwt decode clears stale claims");
  Check(!signer->Sign("input", nullptr), "jwt signer rejects null output");

  const std::string wrong_header =
      zrpc::http::Base64UrlEncode("{\"alg\":\"none\",\"typ\":\"JWT\"}");
  const std::string payload =
      zrpc::http::Base64UrlEncode(
          "{\"sub\":\"player\",\"exp\":4600,\"iat\":1000}");
  const std::string signing_input = wrong_header + "." + payload;
  std::string signature;
  Check(signer->Sign(signing_input, &signature).ok, "jwt custom signature");
  zrpc::http::JwtClaims rejected;
  Check(!codec.Decode(signing_input + "." + signature, &rejected, 1001),
        "jwt algorithm mismatch rejected");

  const std::string valid_header =
      zrpc::http::Base64UrlEncode("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
  const std::string huge_payload =
      zrpc::http::Base64UrlEncode(
          "{\"exp\":999999999999999999999999999999999999}");
  const std::string huge_input = valid_header + "." + huge_payload;
  Check(signer->Sign(huge_input, &signature).ok,
        "jwt huge number signature");
  Check(!codec.Decode(huge_input + "." + signature, &rejected, 1001),
        "jwt overflowing number rejected");

  zrpc::http::JwtClaims negative_exp;
  negative_exp.sub = "player";
  negative_exp.exp = -1;
  Check(!codec.Encode(negative_exp, &token, 1000),
        "jwt negative expiration rejected during encode");

  const auto SignPayload = [&](const std::string &json) {
    const std::string encoded_payload = zrpc::http::Base64UrlEncode(json);
    const std::string custom_input = valid_header + "." + encoded_payload;
    std::string custom_signature;
    Check(signer->Sign(custom_input, &custom_signature).ok,
          "jwt manual payload signature");
    return custom_input + "." + custom_signature;
  };
  Check(!codec.Decode(
            SignPayload("{\"sub\":\"player\",\"iat\":1000}"),
            &rejected, 1001),
        "jwt missing expiration rejected");
  Check(!codec.Decode(
            SignPayload("{\"sub\":\"player\",\"exp\":4600}"),
            &rejected, 1001),
        "jwt missing issued-at rejected");

  zrpc::http::JwtValidationOptions options;
  options.expected_issuer = "trusted-issuer";
  options.expected_audience = "trusted-audience";
  options.max_token_lifetime_seconds = 120;
  zrpc::http::JwtCodec constrained(signer, options);
  zrpc::http::JwtClaims constrained_claims;
  constrained_claims.sub = "player";
  constrained_claims.iss = "trusted-issuer";
  constrained_claims.aud = "trusted-audience";
  constrained_claims.iat = 1000;
  constrained_claims.exp = 1120;
  Check(constrained.Encode(constrained_claims, &token, 1000).ok,
        "jwt configured issuer audience accepted");
  Check(constrained.Decode(token, &decoded, 1001).ok,
        "jwt configured validation succeeds");
  constrained_claims.iss = "untrusted";
  Check(!constrained.Encode(constrained_claims, &token, 1000),
        "jwt issuer mismatch rejected");
  constrained_claims.iss = "trusted-issuer";
  constrained_claims.exp = 1121;
  Check(!constrained.Encode(constrained_claims, &token, 1000),
        "jwt excessive lifetime rejected");
}

void TestAuthAndCryptoBoundaries() {
  zrpc::HttpRequest request;
  request.AddHeader("authorization", "  bearer \tabc  ");
  Check(zrpc::http::AuthMiddleware::ExtractBearerToken(request) == "abc",
        "bearer header case insensitive");

  zrpc::HttpResponse response;
  zrpc::http::AuthMiddleware::WriteUnauthorized(&response, "bad \"token\"\n");
  Check(response.GetBody() == "{\"error\":\"bad \\\"token\\\"\\n\"}",
        "auth error json escaped");

  zrpc::http::ProtectedRoute route(nullptr, {});
  route.Handle(nullptr, request, &response);
  Check(response.GetStatusCode() == zrpc::HttpResponse::k401Unauthorized,
        "null auth backend handled");

  if (!zrpc::http::CryptoBackendAvailable()) {
    return;
  }
  Check(zrpc::http::NewPbkdf2Hasher(1) == nullptr,
        "weak pbkdf2 iterations rejected");
  Check(zrpc::http::NewPbkdf2Hasher(600001) == nullptr,
        "excessive pbkdf2 iterations rejected");
  Check(zrpc::http::NewAesGcmCipher("weak-key") == nullptr,
        "invalid aes key length rejected");
  Check(zrpc::http::NewRsaCipher("not a pem", "") == nullptr,
        "invalid rsa public key rejected");
  auto aes128 = zrpc::http::NewAesGcmCipher(std::string(16, 'k'));
  Check(aes128 != nullptr && aes128->Name() == "AES-128-GCM",
        "aes name matches key size");

  auto signer = zrpc::http::NewHs256Signer(
      "player-auth-secret-with-at-least-32-bytes");
  auto codec = std::make_shared<zrpc::http::JwtCodec>(signer);
  zrpc::http::PlayerAuthService keyless_service(
      codec, zrpc::http::NewPbkdf2Hasher());
  std::string encrypted;
  Check(!keyless_service.EncryptPlayerPayload("payload", &encrypted) &&
            !keyless_service.PayloadCipherStatus(),
        "player payload requires a persistent key by default");

  zrpc::http::PlayerAuthOptions options;
  options.payload_encryption_key = std::string(32, 'p');
  zrpc::http::PlayerAuthService service(
      codec, zrpc::http::NewPbkdf2Hasher(), options);
  std::string decrypted;
  Check(service.EncryptPlayerPayload("payload", &encrypted).ok &&
            service.DecryptPlayerPayload(encrypted, &decrypted).ok &&
            decrypted == "payload",
        "player payload uses the configured persistent key");

  auto account_hasher = zrpc::http::NewPbkdf2Hasher();
  std::string password_hash;
  Check(account_hasher != nullptr &&
            account_hasher->Hash("correct-password", &password_hash),
        "player account password hashed");
  zrpc::http::PlayerAccount account;
  account.player_id = "42";
  account.username = "hero";
  account.password_hash = password_hash;
  account.role = "player";
  service.AddAccount(account);
  std::string access_token;
  zrpc::http::PlayerSession session;
  const zrpc::http::CryptoStatus missing_user =
      service.Login("missing", "wrong-password", &access_token, &session);
  const zrpc::http::CryptoStatus wrong_password =
      service.Login("hero", "wrong-password", &access_token, &session);
  Check(!missing_user && !wrong_password &&
            missing_user.error == wrong_password.error,
        "login failures return a uniform error");

  const int64_t now =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  zrpc::http::JwtClaims wrong_issuer;
  wrong_issuer.sub = "42";
  wrong_issuer.iss = "other";
  wrong_issuer.aud = "game-client";
  wrong_issuer.iat = now;
  wrong_issuer.exp = now + 3600;
  std::string wrong_token;
  Check(codec->Encode(wrong_issuer, &wrong_token, now).ok,
        "player auth wrong issuer token encode");
  Check(!service.VerifyToken(wrong_token, &session),
        "player auth issuer mismatch rejected");
}

void TestHttpClientCompletion() {
  zrpc::EventLoop loop;
  zrpc::HttpClient client(&loop, 2);
  std::atomic<int> invalid_endpoint_count{0};
  client.GetUrl(
      nullptr, 0, "/", "example.test", nullptr, {},
      [&](const std::shared_ptr<zrpc::TcpConnection> &,
          zrpc::HttpResponse &response,
          const std::weak_ptr<zrpc::TcpConnection> &, const std::any &) {
        if (response.GetStatusCode() ==
            zrpc::HttpResponse::k502BadGateway) {
          invalid_endpoint_count.fetch_add(1);
        }
      });
  Check(invalid_endpoint_count.load() == 1,
        "invalid upstream endpoint completes exactly once");

  std::atomic<int> callback_count{0};
  std::atomic<int> terminal_status{0};
  client.GetUrl(
      "127.0.0.1", 1, "/", "example.test", nullptr, {},
      [&](const std::shared_ptr<zrpc::TcpConnection> &,
          zrpc::HttpResponse &response,
          const std::weak_ptr<zrpc::TcpConnection> &, const std::any &) {
        callback_count.fetch_add(1);
        terminal_status.store(static_cast<int>(response.GetStatusCode()));
        loop.Quit();
        throw std::runtime_error("expected callback exception");
      });
  loop.RunAfter(5.0, false, [&]() { loop.Quit(); });
  loop.Run();
  Check(callback_count.load() == 1,
        "http client invokes terminal callback exactly once");
  Check(terminal_status.load() == zrpc::HttpResponse::k502BadGateway ||
            terminal_status.load() == zrpc::HttpResponse::k504GatewayTimeout,
        "http client reports a terminal gateway error");
}

}

int main() {
  TestFragmentedRequest();
  TestInvalidRequests();
  TestFragmentedResponse();
  TestSerialization();
  TestBase64();
  TestJwt();
  TestAuthAndCryptoBoundaries();
  TestHttpClientCompletion();
  if (failures == 0) {
    std::cout << "ALL HTTP REGRESSION TESTS PASSED" << std::endl;
  }
  return failures == 0 ? 0 : 1;
}
