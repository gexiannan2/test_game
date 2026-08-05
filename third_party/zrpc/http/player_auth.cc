#include "zrpc/http/player_auth.h"

#include <chrono>
#include <limits>

namespace zrpc {
namespace http {
namespace {

int64_t NowUnix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

PlayerAuthService::PlayerAuthService(std::shared_ptr<JwtCodec> codec,
                                     std::unique_ptr<PasswordHasher> hasher,
                                     PlayerAuthOptions options)
    : codec_(std::move(codec)),
      hasher_(std::move(hasher)),
      payload_cipher_status_(
          CryptoStatus::Failure("persistent payload encryption key required")),
      options_(std::move(options)) {
  if (hasher_ != nullptr) {
    const CryptoStatus dummy_status =
        hasher_->Hash("zrpc-auth-dummy-password", &dummy_password_hash_);
    if (!dummy_status) {
      dummy_password_hash_.clear();
    }
  }

  std::string payload_key = options_.payload_encryption_key;
  if (payload_key.empty() &&
      options_.allow_ephemeral_payload_encryption_key) {
    const CryptoStatus random_status =
        GenerateSecureRandom(32, &payload_key);
    if (!random_status) {
      payload_cipher_status_ = random_status;
      return;
    }
  }
  if (payload_key.empty()) {
    return;
  }
  if (payload_key.size() == 16 || payload_key.size() == 24 ||
      payload_key.size() == 32) {
    payload_cipher_ = NewAesGcmCipher(payload_key);
    if (payload_cipher_ != nullptr) {
      payload_cipher_status_ = CryptoStatus::Success();
      return;
    }
  }
  payload_cipher_status_ =
      CryptoStatus::Failure("payload encryption key must be 16/24/32 bytes");
}

void PlayerAuthService::AddAccount(const PlayerAccount& account) {
  std::lock_guard<std::mutex> lock(accounts_mutex_);
  accounts_by_username_[account.username] = account;
}

CryptoStatus PlayerAuthService::Login(const std::string& username,
                                      const std::string& password,
                                      std::string* access_token,
                                      PlayerSession* session) {
  if (codec_ == nullptr || hasher_ == nullptr || access_token == nullptr ||
      session == nullptr || dummy_password_hash_.empty()) {
    return CryptoStatus::Failure("player auth unavailable");
  }
  access_token->clear();
  *session = PlayerSession();

  PlayerAccount account;
  bool account_found = false;
  {
    std::lock_guard<std::mutex> lock(accounts_mutex_);
    const auto it = accounts_by_username_.find(username);
    if (it != accounts_by_username_.end()) {
      account = it->second;
      account_found = true;
    }
  }

  const CryptoStatus verify =
      hasher_->Verify(password, account_found ? account.password_hash
                                              : dummy_password_hash_);
  if (!account_found || !verify) {
    return CryptoStatus::Failure("invalid username or password");
  }

  const int64_t now = NowUnix();
  if (options_.token_ttl_seconds <= 0 ||
      options_.token_ttl_seconds >
          std::numeric_limits<int64_t>::max() - now) {
    return CryptoStatus::Failure("invalid token ttl");
  }
  JwtClaims claims;
  claims.sub = account.player_id;
  claims.iss = options_.issuer;
  claims.aud = options_.audience;
  claims.iat = now;
  claims.exp = now + options_.token_ttl_seconds;
  claims.Set("username", account.username);
  claims.Set("role", account.role);
  claims.Set("player_id", account.player_id);

  const CryptoStatus encoded = codec_->Encode(claims, access_token, now);
  if (!encoded) {
    return encoded;
  }

  session->player_id = account.player_id;
  session->username = account.username;
  session->role = account.role;
  session->exp = claims.exp;
  return CryptoStatus::Success();
}

CryptoStatus PlayerAuthService::VerifyToken(const std::string& bearer_token,
                                            PlayerSession* session) const {
  if (codec_ == nullptr || session == nullptr) {
    return CryptoStatus::Failure("player auth unavailable");
  }

  JwtClaims claims;
  const CryptoStatus decoded = codec_->Decode(bearer_token, &claims);
  if (!decoded) {
    return decoded;
  }

  if ((!options_.issuer.empty() && claims.iss != options_.issuer) ||
      (!options_.audience.empty() && claims.aud != options_.audience)) {
    return CryptoStatus::Failure("jwt issuer or audience mismatch");
  }

  PlayerSession verified;
  verified.player_id = claims.Get("player_id");
  if (verified.player_id.empty()) {
    verified.player_id = claims.sub;
  }
  if (verified.player_id.empty()) {
    return CryptoStatus::Failure("jwt missing player id");
  }
  verified.username = claims.Get("username");
  verified.role = claims.Get("role");
  verified.exp = claims.exp;
  *session = std::move(verified);
  return CryptoStatus::Success();
}

CryptoStatus PlayerAuthService::EncryptPlayerPayload(
    const std::string& plaintext, std::string* ciphertext) const {
  if (payload_cipher_ == nullptr) {
    return payload_cipher_status_;
  }
  return payload_cipher_->Encrypt(plaintext, ciphertext, "player-payload");
}

CryptoStatus PlayerAuthService::DecryptPlayerPayload(
    const std::string& ciphertext, std::string* plaintext) const {
  if (payload_cipher_ == nullptr) {
    return payload_cipher_status_;
  }
  return payload_cipher_->Decrypt(ciphertext, plaintext, "player-payload");
}

}  // namespace http
}  // namespace zrpc
