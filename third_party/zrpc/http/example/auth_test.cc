#include <iostream>
#include <memory>
#include <string>

#include "zrpc/http/crypto.h"
#include "zrpc/http/jwt.h"
#include "zrpc/http/player_auth.h"

namespace {

int TestJwtHs256RoundTrip() {
  if (!zrpc::http::CryptoBackendAvailable()) {
    std::cout << "SKIP TestJwtHs256RoundTrip (openssl unavailable)" << std::endl;
    return 0;
  }

  auto signer = zrpc::http::NewHs256Signer(
      "unit-test-secret-with-at-least-32-bytes");
  zrpc::http::JwtCodec codec(signer);

  zrpc::http::JwtClaims claims;
  claims.sub = "player-1";
  claims.iss = "zrpc-test";
  claims.exp = 1'700'003'600;
  claims.Set("username", "alice");

  std::string token;
  if (!codec.Encode(claims, &token, 1'700'000'000)) {
    std::cerr << "FAIL TestJwtHs256RoundTrip encode" << std::endl;
    return 1;
  }

  zrpc::http::JwtClaims decoded;
  if (!codec.Decode(token, &decoded, 1'700'000'000)) {
    std::cerr << "FAIL TestJwtHs256RoundTrip decode" << std::endl;
    return 1;
  }
  if (decoded.sub != "player-1" || decoded.Get("username") != "alice") {
    std::cerr << "FAIL TestJwtHs256RoundTrip payload" << std::endl;
    return 1;
  }

  std::cout << "PASS TestJwtHs256RoundTrip" << std::endl;
  return 0;
}

int TestPlayerLogin() {
  if (!zrpc::http::CryptoBackendAvailable()) {
    std::cout << "SKIP TestPlayerLogin (openssl unavailable)" << std::endl;
    return 0;
  }

  auto signer = zrpc::http::NewHs256Signer(
      "login-test-secret-with-at-least-32-bytes");
  auto codec = std::make_shared<zrpc::http::JwtCodec>(signer);
  auto hasher = zrpc::http::NewPbkdf2Hasher();
  if (hasher == nullptr) {
    std::cerr << "FAIL TestPlayerLogin hasher unavailable" << std::endl;
    return 1;
  }

  std::string encoded;
  if (!hasher->Hash("pass123", &encoded)) {
    std::cerr << "FAIL TestPlayerLogin hash" << std::endl;
    return 1;
  }

  zrpc::http::PlayerAuthService auth(codec, std::move(hasher));

  zrpc::http::PlayerAccount account;
  account.player_id = "42";
  account.username = "hero";
  account.password_hash = encoded;
  account.role = "player";
  auth.AddAccount(account);

  std::string token;
  zrpc::http::PlayerSession session;
  if (!auth.Login("hero", "pass123", &token, &session)) {
    std::cerr << "FAIL TestPlayerLogin login" << std::endl;
    return 1;
  }
  if (session.player_id != "42" || token.empty()) {
    std::cerr << "FAIL TestPlayerLogin session" << std::endl;
    return 1;
  }

  zrpc::http::PlayerSession verified;
  if (!auth.VerifyToken(token, &verified) || verified.username != "hero") {
    std::cerr << "FAIL TestPlayerLogin verify" << std::endl;
    return 1;
  }

  std::cout << "PASS TestPlayerLogin" << std::endl;
  return 0;
}

int TestSymmetricCipher() {
  if (!zrpc::http::CryptoBackendAvailable()) {
    std::cout << "SKIP TestSymmetricCipher (openssl unavailable)" << std::endl;
    return 0;
  }

  auto cipher = zrpc::http::NewAesGcmCipher(std::string(32, 'k'));
  std::string encrypted;
  std::string decrypted;
  if (!cipher->Encrypt("player inventory", &encrypted, "game")) {
    std::cerr << "FAIL TestSymmetricCipher encrypt" << std::endl;
    return 1;
  }
  if (!cipher->Decrypt(encrypted, &decrypted, "game") ||
      decrypted != "player inventory") {
    std::cerr << "FAIL TestSymmetricCipher decrypt" << std::endl;
    return 1;
  }

  std::cout << "PASS TestSymmetricCipher" << std::endl;
  return 0;
}

}  // namespace

int main() {
  int failed = 0;
  failed += TestJwtHs256RoundTrip();
  failed += TestSymmetricCipher();
  failed += TestPlayerLogin();

  if (failed == 0) {
    std::cout << "ALL TESTS PASSED" << std::endl;
  }
  return failed;
}
