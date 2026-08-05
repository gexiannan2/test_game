#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "zrpc/http/crypto.h"
#include "zrpc/http/jwt.h"

namespace zrpc {
namespace http {

struct PlayerSession {
  std::string player_id;
  std::string username;
  std::string role;
  int64_t exp = 0;
};

struct PlayerAccount {
  std::string player_id;
  std::string username;
  std::string password_hash;
  std::string role;
};

struct PlayerAuthOptions {
  std::string issuer = "zrpc-http";
  std::string audience = "game-client";
  int64_t token_ttl_seconds = 3600;
  std::string payload_encryption_key;
  bool allow_ephemeral_payload_encryption_key = false;
};

class PlayerAuthService {
 public:
  PlayerAuthService(std::shared_ptr<JwtCodec> codec,
                    std::unique_ptr<PasswordHasher> hasher,
                    PlayerAuthOptions options = {});

  void AddAccount(const PlayerAccount& account);
  CryptoStatus Login(const std::string& username, const std::string& password,
                     std::string* access_token, PlayerSession* session);
  CryptoStatus VerifyToken(const std::string& bearer_token,
                           PlayerSession* session) const;
  CryptoStatus EncryptPlayerPayload(const std::string& plaintext,
                                    std::string* ciphertext) const;
  CryptoStatus DecryptPlayerPayload(const std::string& ciphertext,
                                    std::string* plaintext) const;

  const JwtCodec* codec() const { return codec_.get(); }
  const CryptoStatus& PayloadCipherStatus() const {
    return payload_cipher_status_;
  }

 private:
  std::shared_ptr<JwtCodec> codec_;
  std::unique_ptr<PasswordHasher> hasher_;
  std::unique_ptr<SymmetricCipher> payload_cipher_;
  CryptoStatus payload_cipher_status_;
  PlayerAuthOptions options_;
  std::string dummy_password_hash_;
  mutable std::mutex accounts_mutex_;
  std::map<std::string, PlayerAccount> accounts_by_username_;
};

}  // namespace http
}  // namespace zrpc
