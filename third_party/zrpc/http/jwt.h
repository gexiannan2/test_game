#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "zrpc/http/crypto.h"

namespace zrpc {
namespace http {

struct JwtClaims {
  std::string sub;
  std::string iss;
  std::string aud;
  int64_t exp = 0;
  int64_t iat = 0;
  int64_t nbf = 0;
  std::map<std::string, std::string> custom;

  bool IsExpired(int64_t now_unix) const;
  std::string Get(const std::string& key) const;
  void Set(const std::string& key, const std::string& value);
};

class JwtSigner {
 public:
  virtual ~JwtSigner() = default;
  virtual std::string Algorithm() const = 0;
  virtual CryptoStatus Sign(const std::string& signing_input,
                            std::string* signature) = 0;
  virtual CryptoStatus Verify(const std::string& signing_input,
                              const std::string& signature) = 0;
};

class Hs256Signer : public JwtSigner {
 public:
  explicit Hs256Signer(std::string secret);
  std::string Algorithm() const override;
  CryptoStatus Sign(const std::string& signing_input,
                    std::string* signature) override;
  CryptoStatus Verify(const std::string& signing_input,
                      const std::string& signature) override;

 private:
  std::string secret_;
};

class Rs256Signer : public JwtSigner {
 public:
  explicit Rs256Signer(std::shared_ptr<AsymmetricCipher> rsa);
  std::string Algorithm() const override;
  CryptoStatus Sign(const std::string& signing_input,
                    std::string* signature) override;
  CryptoStatus Verify(const std::string& signing_input,
                      const std::string& signature) override;

 private:
  std::shared_ptr<AsymmetricCipher> rsa_;
};

struct JwtValidationOptions {
  bool require_expiration = true;
  bool require_issued_at = true;
  int64_t clock_skew_seconds = 30;
  int64_t max_token_lifetime_seconds = 24 * 60 * 60;
  std::string expected_issuer;
  std::string expected_audience;
};

class JwtCodec {
 public:
  explicit JwtCodec(std::shared_ptr<JwtSigner> signer,
                    JwtValidationOptions options = {});

  CryptoStatus Encode(const JwtClaims& claims, std::string* token,
                      int64_t now_unix = 0) const;
  CryptoStatus Decode(const std::string& token, JwtClaims* claims,
                      int64_t now_unix = 0) const;

  const JwtSigner* signer() const { return signer_.get(); }
  const JwtValidationOptions& options() const { return options_; }

 private:
  std::shared_ptr<JwtSigner> signer_;
  JwtValidationOptions options_;
};

std::shared_ptr<JwtSigner> NewHs256Signer(const std::string& secret);
std::shared_ptr<JwtSigner> NewRs256Signer(
    std::shared_ptr<AsymmetricCipher> rsa);
std::shared_ptr<JwtSigner> NewRs256Signer(
    std::unique_ptr<AsymmetricCipher> rsa);

}  // namespace http
}  // namespace zrpc
