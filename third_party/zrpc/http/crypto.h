#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zrpc {
namespace http {

constexpr size_t kMinHmacSha256KeyBytes = 32;
constexpr uint32_t kMinPbkdf2Iterations = 100000;
constexpr uint32_t kMaxPbkdf2Iterations = 600000;
constexpr int kMinRsaBits = 2048;

struct CryptoStatus {
  bool ok = false;
  std::string error;

  static CryptoStatus Success() { return {true, {}}; }
  static CryptoStatus Failure(std::string message) {
    return {false, std::move(message)};
  }

  explicit operator bool() const { return ok; }
};

std::string Base64UrlEncode(const std::string& input);
std::string Base64UrlEncode(const uint8_t* data, size_t len);
bool Base64UrlDecode(const std::string& input, std::string* output);

class SymmetricCipher {
 public:
  virtual ~SymmetricCipher() = default;
  virtual std::string Name() const = 0;
  virtual CryptoStatus Encrypt(const std::string& plaintext,
                               std::string* ciphertext,
                               const std::string& aad = "") = 0;
  virtual CryptoStatus Decrypt(const std::string& ciphertext,
                               std::string* plaintext,
                               const std::string& aad = "") = 0;
};

class AsymmetricCipher {
 public:
  virtual ~AsymmetricCipher() = default;
  virtual std::string Name() const = 0;
  virtual CryptoStatus Encrypt(const std::string& plaintext,
                               std::string* ciphertext) = 0;
  virtual CryptoStatus Decrypt(const std::string& ciphertext,
                               std::string* plaintext) = 0;
  virtual CryptoStatus Sign(const std::string& message,
                            std::string* signature) = 0;
  virtual CryptoStatus Verify(const std::string& message,
                              const std::string& signature) = 0;
};

class PasswordHasher {
 public:
  virtual ~PasswordHasher() = default;
  virtual std::string Name() const = 0;
  virtual CryptoStatus Hash(const std::string& password,
                            std::string* encoded) = 0;
  virtual CryptoStatus Verify(const std::string& password,
                              const std::string& encoded) = 0;
};

std::unique_ptr<SymmetricCipher> NewAesGcmCipher(const std::string& key);
std::unique_ptr<AsymmetricCipher> NewRsaCipher(const std::string& public_pem,
                                              const std::string& private_pem);
std::unique_ptr<PasswordHasher> NewPbkdf2Hasher(uint32_t iterations = 120000);

bool CryptoBackendAvailable();

CryptoStatus GenerateSecureRandom(size_t size, std::string* output);
CryptoStatus HmacSha256Sign(const std::string& key, const std::string& message,
                            std::string* mac);

}  // namespace http
}  // namespace zrpc
