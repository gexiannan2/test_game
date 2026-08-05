#include "zrpc/http/crypto.h"

#ifdef ZRPC_USE_OPENSSL

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <charconv>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace zrpc {
namespace http {
namespace {

constexpr size_t kAesGcmNonceSize = 12;
constexpr size_t kAesGcmTagSize = 16;
constexpr size_t kMaxPasswordBytes = 4096;

bool SecureEqual(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char result = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    result |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return result == 0;
}

bool IsValidRsaKey(EVP_PKEY* key) {
  return key != nullptr && EVP_PKEY_base_id(key) == EVP_PKEY_RSA &&
         EVP_PKEY_bits(key) >= kMinRsaBits;
}

bool IsValidPublicRsaPem(const std::string& pem) {
  if (pem.empty() ||
      pem.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) {
    return false;
  }
  EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  const bool valid = IsValidRsaKey(key);
  EVP_PKEY_free(key);
  return valid;
}

bool IsValidPrivateRsaPem(const std::string& pem) {
  if (pem.empty() ||
      pem.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (bio == nullptr) {
    return false;
  }
  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  const bool valid = IsValidRsaKey(key);
  EVP_PKEY_free(key);
  return valid;
}

class AesGcmCipher : public SymmetricCipher {
 public:
  explicit AesGcmCipher(std::string key) : key_(std::move(key)) {}

  std::string Name() const override {
    if (key_.size() == 16) {
      return "AES-128-GCM";
    }
    if (key_.size() == 24) {
      return "AES-192-GCM";
    }
    return "AES-256-GCM";
  }

  CryptoStatus Encrypt(const std::string& plaintext, std::string* ciphertext,
                       const std::string& aad) override {
    if (ciphertext == nullptr) {
      return CryptoStatus::Failure("null ciphertext output");
    }
    if (key_.size() != 16 && key_.size() != 24 && key_.size() != 32) {
      return CryptoStatus::Failure("aes key must be 16/24/32 bytes");
    }
    if (plaintext.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        aad.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return CryptoStatus::Failure("aes input too large");
    }

    std::vector<uint8_t> nonce(kAesGcmNonceSize);
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
      return CryptoStatus::Failure("failed to generate nonce");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
      return CryptoStatus::Failure("EVP_CIPHER_CTX_new failed");
    }

    const EVP_CIPHER* cipher = nullptr;
    if (key_.size() == 16) {
      cipher = EVP_aes_128_gcm();
    } else if (key_.size() == 24) {
      cipher = EVP_aes_192_gcm();
    } else {
      cipher = EVP_aes_256_gcm();
    }

    CryptoStatus status = CryptoStatus::Success();
    std::vector<uint8_t> encrypted(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;
    int total_len = 0;

    do {
      if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1) {
        status = CryptoStatus::Failure("EVP_EncryptInit_ex failed");
        break;
      }
      if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                              static_cast<int>(nonce.size()), nullptr) != 1) {
        status = CryptoStatus::Failure("set iv len failed");
        break;
      }
      if (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                             reinterpret_cast<const uint8_t*>(key_.data()),
                             nonce.data()) != 1) {
        status = CryptoStatus::Failure("EVP_EncryptInit_ex key failed");
        break;
      }
      if (!aad.empty() &&
          EVP_EncryptUpdate(ctx, nullptr, &out_len,
                            reinterpret_cast<const uint8_t*>(aad.data()),
                            static_cast<int>(aad.size())) != 1) {
        status = CryptoStatus::Failure("aad update failed");
        break;
      }
      if (EVP_EncryptUpdate(
              ctx, encrypted.data(), &out_len,
              reinterpret_cast<const uint8_t*>(plaintext.data()),
              static_cast<int>(plaintext.size())) != 1) {
        status = CryptoStatus::Failure("encrypt update failed");
        break;
      }
      total_len = out_len;
      if (EVP_EncryptFinal_ex(ctx, encrypted.data() + total_len, &out_len) != 1) {
        status = CryptoStatus::Failure("encrypt final failed");
        break;
      }
      total_len += out_len;

      std::vector<uint8_t> tag(kAesGcmTagSize);
      if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()),
                              tag.data()) != 1) {
        status = CryptoStatus::Failure("get gcm tag failed");
        break;
      }

      std::string packed;
      packed.reserve(nonce.size() + total_len + tag.size());
      packed.append(reinterpret_cast<const char*>(nonce.data()), nonce.size());
      packed.append(reinterpret_cast<const char*>(encrypted.data()), total_len);
      packed.append(reinterpret_cast<const char*>(tag.data()), tag.size());
      *ciphertext = Base64UrlEncode(packed);
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return status;
  }

  CryptoStatus Decrypt(const std::string& ciphertext, std::string* plaintext,
                       const std::string& aad) override {
    if (plaintext == nullptr) {
      return CryptoStatus::Failure("null plaintext output");
    }
    std::string packed;
    if (!Base64UrlDecode(ciphertext, &packed)) {
      return CryptoStatus::Failure("invalid ciphertext encoding");
    }
    if (packed.size() < kAesGcmNonceSize + kAesGcmTagSize) {
      return CryptoStatus::Failure("ciphertext too short");
    }

    const uint8_t* data = reinterpret_cast<const uint8_t*>(packed.data());
    const size_t cipher_len =
        packed.size() - kAesGcmNonceSize - kAesGcmTagSize;
    if (cipher_len > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        aad.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return CryptoStatus::Failure("aes input too large");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
      return CryptoStatus::Failure("EVP_CIPHER_CTX_new failed");
    }

    const EVP_CIPHER* cipher = nullptr;
    if (key_.size() == 16) {
      cipher = EVP_aes_128_gcm();
    } else if (key_.size() == 24) {
      cipher = EVP_aes_192_gcm();
    } else if (key_.size() == 32) {
      cipher = EVP_aes_256_gcm();
    } else {
      EVP_CIPHER_CTX_free(ctx);
      return CryptoStatus::Failure("aes key must be 16/24/32 bytes");
    }

    CryptoStatus status = CryptoStatus::Success();
    std::vector<uint8_t> out(cipher_len + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;
    int total_len = 0;

    do {
      if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1) {
        status = CryptoStatus::Failure("EVP_DecryptInit_ex failed");
        break;
      }
      if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kAesGcmNonceSize,
                              nullptr) != 1) {
        status = CryptoStatus::Failure("set iv len failed");
        break;
      }
      if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                             reinterpret_cast<const uint8_t*>(key_.data()),
                             data) != 1) {
        status = CryptoStatus::Failure("EVP_DecryptInit_ex key failed");
        break;
      }
      if (!aad.empty() &&
          EVP_DecryptUpdate(ctx, nullptr, &out_len, data + kAesGcmNonceSize, 0) !=
              1) {
        status = CryptoStatus::Failure("aad update failed");
        break;
      }
      if (!aad.empty() &&
          EVP_DecryptUpdate(ctx, nullptr, &out_len,
                            reinterpret_cast<const uint8_t*>(aad.data()),
                            static_cast<int>(aad.size())) != 1) {
        status = CryptoStatus::Failure("aad update failed");
        break;
      }
      if (EVP_DecryptUpdate(ctx, out.data(), &out_len, data + kAesGcmNonceSize,
                            static_cast<int>(cipher_len)) != 1) {
        status = CryptoStatus::Failure("decrypt update failed");
        break;
      }
      total_len = out_len;
      const uint8_t* tag = data + kAesGcmNonceSize + cipher_len;
      if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kAesGcmTagSize,
                              const_cast<uint8_t*>(tag)) != 1) {
        status = CryptoStatus::Failure("set gcm tag failed");
        break;
      }
      if (EVP_DecryptFinal_ex(ctx, out.data() + total_len, &out_len) != 1) {
        status = CryptoStatus::Failure("decrypt auth failed");
        break;
      }
      total_len += out_len;
      plaintext->assign(reinterpret_cast<const char*>(out.data()), total_len);
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return status;
  }

 private:
  std::string key_;
};

class RsaCipher : public AsymmetricCipher {
 public:
  RsaCipher(std::string public_pem, std::string private_pem)
      : public_pem_(std::move(public_pem)), private_pem_(std::move(private_pem)) {}

  std::string Name() const override { return "RSA"; }

  CryptoStatus Encrypt(const std::string& plaintext,
                       std::string* ciphertext) override {
    if (ciphertext == nullptr) {
      return CryptoStatus::Failure("null ciphertext output");
    }
    if (public_pem_.empty()) {
      return CryptoStatus::Failure("missing rsa public key");
    }
    if (public_pem_.size() >
        static_cast<size_t>(std::numeric_limits<int>::max())) {
      return CryptoStatus::Failure("rsa public key too large");
    }

    BIO* bio =
        BIO_new_mem_buf(public_pem_.data(), static_cast<int>(public_pem_.size()));
    if (bio == nullptr) {
      return CryptoStatus::Failure("BIO_new_mem_buf failed");
    }
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (pkey == nullptr) {
      return CryptoStatus::Failure("PEM_read_bio_PUBKEY failed");
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    EVP_PKEY_free(pkey);
    if (ctx == nullptr) {
      return CryptoStatus::Failure("EVP_PKEY_CTX_new failed");
    }

    CryptoStatus status = CryptoStatus::Success();
    size_t out_len = 0;
    std::string encrypted;

    do {
      if (EVP_PKEY_encrypt_init(ctx) <= 0) {
        status = CryptoStatus::Failure("EVP_PKEY_encrypt_init failed");
        break;
      }
      if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
        status = CryptoStatus::Failure("set rsa padding failed");
        break;
      }
      if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) <= 0 ||
          EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) <= 0) {
        status = CryptoStatus::Failure("set rsa oaep digest failed");
        break;
      }
      if (EVP_PKEY_encrypt(ctx, nullptr, &out_len,
                           reinterpret_cast<const uint8_t*>(plaintext.data()),
                           plaintext.size()) <= 0) {
        status = CryptoStatus::Failure("EVP_PKEY_encrypt size failed");
        break;
      }
      encrypted.resize(out_len);
      if (EVP_PKEY_encrypt(
              ctx, reinterpret_cast<uint8_t*>(&encrypted[0]), &out_len,
              reinterpret_cast<const uint8_t*>(plaintext.data()),
              plaintext.size()) <= 0) {
        status = CryptoStatus::Failure("EVP_PKEY_encrypt failed");
        break;
      }
      encrypted.resize(out_len);
      *ciphertext = Base64UrlEncode(encrypted);
    } while (false);

    EVP_PKEY_CTX_free(ctx);
    return status;
  }

  CryptoStatus Decrypt(const std::string& ciphertext,
                       std::string* plaintext) override {
    if (plaintext == nullptr) {
      return CryptoStatus::Failure("null plaintext output");
    }
    if (private_pem_.empty()) {
      return CryptoStatus::Failure("missing rsa private key");
    }
    if (private_pem_.size() >
        static_cast<size_t>(std::numeric_limits<int>::max())) {
      return CryptoStatus::Failure("rsa private key too large");
    }

    std::string encrypted;
    if (!Base64UrlDecode(ciphertext, &encrypted)) {
      return CryptoStatus::Failure("invalid ciphertext encoding");
    }

    BIO* bio = BIO_new_mem_buf(private_pem_.data(),
                               static_cast<int>(private_pem_.size()));
    if (bio == nullptr) {
      return CryptoStatus::Failure("BIO_new_mem_buf failed");
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (pkey == nullptr) {
      return CryptoStatus::Failure("PEM_read_bio_PrivateKey failed");
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    EVP_PKEY_free(pkey);
    if (ctx == nullptr) {
      return CryptoStatus::Failure("EVP_PKEY_CTX_new failed");
    }

    CryptoStatus status = CryptoStatus::Success();
    size_t out_len = 0;
    std::string decrypted;

    do {
      if (EVP_PKEY_decrypt_init(ctx) <= 0) {
        status = CryptoStatus::Failure("EVP_PKEY_decrypt_init failed");
        break;
      }
      if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
        status = CryptoStatus::Failure("set rsa padding failed");
        break;
      }
      if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) <= 0 ||
          EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) <= 0) {
        status = CryptoStatus::Failure("set rsa oaep digest failed");
        break;
      }
      if (EVP_PKEY_decrypt(ctx, nullptr, &out_len,
                           reinterpret_cast<const uint8_t*>(encrypted.data()),
                           encrypted.size()) <= 0) {
        status = CryptoStatus::Failure("EVP_PKEY_decrypt size failed");
        break;
      }
      decrypted.resize(out_len);
      if (EVP_PKEY_decrypt(
              ctx, reinterpret_cast<uint8_t*>(&decrypted[0]), &out_len,
              reinterpret_cast<const uint8_t*>(encrypted.data()),
              encrypted.size()) <= 0) {
        status = CryptoStatus::Failure("EVP_PKEY_decrypt failed");
        break;
      }
      decrypted.resize(out_len);
      *plaintext = decrypted;
    } while (false);

    EVP_PKEY_CTX_free(ctx);
    return status;
  }

  CryptoStatus Sign(const std::string& message, std::string* signature) override {
    if (signature == nullptr) {
      return CryptoStatus::Failure("null signature output");
    }
    if (private_pem_.empty()) {
      return CryptoStatus::Failure("missing rsa private key");
    }
    if (private_pem_.size() >
        static_cast<size_t>(std::numeric_limits<int>::max())) {
      return CryptoStatus::Failure("rsa private key too large");
    }

    BIO* bio = BIO_new_mem_buf(private_pem_.data(),
                               static_cast<int>(private_pem_.size()));
    if (bio == nullptr) {
      return CryptoStatus::Failure("BIO_new_mem_buf failed");
    }
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (pkey == nullptr) {
      return CryptoStatus::Failure("PEM_read_bio_PrivateKey failed");
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
      EVP_PKEY_free(pkey);
      return CryptoStatus::Failure("EVP_MD_CTX_new failed");
    }

    CryptoStatus status = CryptoStatus::Success();
    size_t sig_len = 0;
    std::vector<uint8_t> sig;

    do {
      if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1) {
        status = CryptoStatus::Failure("EVP_DigestSignInit failed");
        break;
      }
      if (EVP_DigestSignUpdate(
              ctx, message.data(), message.size()) != 1) {
        status = CryptoStatus::Failure("EVP_DigestSignUpdate failed");
        break;
      }
      if (EVP_DigestSignFinal(ctx, nullptr, &sig_len) != 1) {
        status = CryptoStatus::Failure("EVP_DigestSignFinal size failed");
        break;
      }
      sig.resize(sig_len);
      if (EVP_DigestSignFinal(ctx, sig.data(), &sig_len) != 1) {
        status = CryptoStatus::Failure("EVP_DigestSignFinal failed");
        break;
      }
      sig.resize(sig_len);
      *signature = std::string(reinterpret_cast<const char*>(sig.data()), sig.size());
    } while (false);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return status;
  }

  CryptoStatus Verify(const std::string& message,
                      const std::string& signature) override {
    if (public_pem_.empty()) {
      return CryptoStatus::Failure("missing rsa public key");
    }
    if (public_pem_.size() >
        static_cast<size_t>(std::numeric_limits<int>::max())) {
      return CryptoStatus::Failure("rsa public key too large");
    }

    BIO* bio =
        BIO_new_mem_buf(public_pem_.data(), static_cast<int>(public_pem_.size()));
    if (bio == nullptr) {
      return CryptoStatus::Failure("BIO_new_mem_buf failed");
    }
    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (pkey == nullptr) {
      return CryptoStatus::Failure("PEM_read_bio_PUBKEY failed");
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
      EVP_PKEY_free(pkey);
      return CryptoStatus::Failure("EVP_MD_CTX_new failed");
    }

    CryptoStatus status = CryptoStatus::Success();
    do {
      if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1) {
        status = CryptoStatus::Failure("EVP_DigestVerifyInit failed");
        break;
      }
      if (EVP_DigestVerifyUpdate(ctx, message.data(), message.size()) != 1) {
        status = CryptoStatus::Failure("EVP_DigestVerifyUpdate failed");
        break;
      }
      if (EVP_DigestVerifyFinal(
              ctx, reinterpret_cast<const uint8_t*>(signature.data()),
              signature.size()) != 1) {
        status = CryptoStatus::Failure("signature verify failed");
      }
    } while (false);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return status;
  }

 private:
  std::string public_pem_;
  std::string private_pem_;
};

class Pbkdf2Hasher : public PasswordHasher {
 public:
  explicit Pbkdf2Hasher(uint32_t iterations) : iterations_(iterations) {}

  std::string Name() const override { return "PBKDF2-HMAC-SHA256"; }

  CryptoStatus Hash(const std::string& password, std::string* encoded) override {
    if (encoded == nullptr) {
      return CryptoStatus::Failure("null encoded output");
    }
    if (iterations_ < kMinPbkdf2Iterations ||
        iterations_ > kMaxPbkdf2Iterations) {
      return CryptoStatus::Failure("invalid pbkdf2 iterations");
    }
    if (password.size() > kMaxPasswordBytes) {
      return CryptoStatus::Failure("password too large");
    }

    std::vector<uint8_t> salt(16);
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) {
      return CryptoStatus::Failure("failed to generate salt");
    }

    std::vector<uint8_t> hash(32);
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          static_cast<int>(iterations_), EVP_sha256(),
                          static_cast<int>(hash.size()), hash.data()) != 1) {
      return CryptoStatus::Failure("PBKDF2 failed");
    }

    *encoded = "pbkdf2$" + std::to_string(iterations_) + "$" +
               Base64UrlEncode(salt.data(), salt.size()) + "$" +
               Base64UrlEncode(hash.data(), hash.size());
    return CryptoStatus::Success();
  }

  CryptoStatus Verify(const std::string& password,
                      const std::string& encoded) override {
    if (password.size() > kMaxPasswordBytes || encoded.size() > 256) {
      return CryptoStatus::Failure("pbkdf2 input too large");
    }
    const size_t p1 = encoded.find('$');
    const size_t p2 = encoded.find('$', p1 + 1);
    const size_t p3 = encoded.find('$', p2 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos ||
        p3 == std::string::npos || encoded.compare(0, p1, "pbkdf2") != 0) {
      return CryptoStatus::Failure("invalid pbkdf2 format");
    }

    uint32_t iterations = 0;
    const char* iterations_begin = encoded.data() + p1 + 1;
    const char* iterations_end = encoded.data() + p2;
    const auto parse_result =
        std::from_chars(iterations_begin, iterations_end, iterations);
    if (parse_result.ec != std::errc() ||
        parse_result.ptr != iterations_end ||
        iterations < kMinPbkdf2Iterations ||
        iterations > kMaxPbkdf2Iterations) {
      return CryptoStatus::Failure("invalid pbkdf2 iterations");
    }
    std::string salt;
    std::string expected;
    if (!Base64UrlDecode(encoded.substr(p2 + 1, p3 - p2 - 1), &salt) ||
        !Base64UrlDecode(encoded.substr(p3 + 1), &expected)) {
      return CryptoStatus::Failure("invalid pbkdf2 encoding");
    }
    if (salt.size() != 16 || expected.size() != 32) {
      return CryptoStatus::Failure("invalid pbkdf2 length");
    }

    std::vector<uint8_t> hash(expected.size());
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          reinterpret_cast<const uint8_t*>(salt.data()),
                          static_cast<int>(salt.size()),
                          static_cast<int>(iterations), EVP_sha256(),
                          static_cast<int>(hash.size()), hash.data()) != 1) {
      return CryptoStatus::Failure("PBKDF2 verify failed");
    }

    const std::string actual(reinterpret_cast<const char*>(hash.data()),
                             hash.size());
    if (!SecureEqual(actual, expected)) {
      return CryptoStatus::Failure("password mismatch");
    }
    return CryptoStatus::Success();
  }

 private:
  uint32_t iterations_;
};

CryptoStatus HmacSha256(const std::string& key, const std::string& message,
                        std::string* mac) {
  if (mac == nullptr) {
    return CryptoStatus::Failure("null mac output");
  }
  if (key.size() < kMinHmacSha256KeyBytes) {
    return CryptoStatus::Failure("hmac key must contain at least 32 bytes");
  }
  if (key.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return CryptoStatus::Failure("hmac key too large");
  }

  unsigned int len = 0;
  std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
  unsigned char* result = HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                               reinterpret_cast<const unsigned char*>(message.data()),
                               message.size(), out.data(), &len);
  if (result == nullptr) {
    return CryptoStatus::Failure("HMAC failed");
  }
  mac->assign(reinterpret_cast<const char*>(out.data()), len);
  return CryptoStatus::Success();
}

}  // namespace

std::unique_ptr<SymmetricCipher> NewAesGcmCipher(const std::string& key) {
  if (key.size() != 16 && key.size() != 24 && key.size() != 32) {
    return nullptr;
  }
  return std::make_unique<AesGcmCipher>(key);
}

std::unique_ptr<AsymmetricCipher> NewRsaCipher(const std::string& public_pem,
                                                const std::string& private_pem) {
  if ((public_pem.empty() && private_pem.empty()) ||
      (!public_pem.empty() && !IsValidPublicRsaPem(public_pem)) ||
      (!private_pem.empty() && !IsValidPrivateRsaPem(private_pem))) {
    return nullptr;
  }
  return std::make_unique<RsaCipher>(public_pem, private_pem);
}

std::unique_ptr<PasswordHasher> NewPbkdf2Hasher(uint32_t iterations) {
  if (iterations < kMinPbkdf2Iterations ||
      iterations > kMaxPbkdf2Iterations) {
    return nullptr;
  }
  return std::make_unique<Pbkdf2Hasher>(iterations);
}

CryptoStatus GenerateSecureRandom(size_t size, std::string* output) {
  if (output == nullptr) {
    return CryptoStatus::Failure("null random output");
  }
  if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return CryptoStatus::Failure("random output too large");
  }
  if (size == 0) {
    output->clear();
    return CryptoStatus::Success();
  }
  std::string random(size, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(&random[0]),
                 static_cast<int>(random.size())) != 1) {
    return CryptoStatus::Failure("failed to generate random bytes");
  }
  *output = std::move(random);
  return CryptoStatus::Success();
}

CryptoStatus HmacSha256Sign(const std::string& key, const std::string& message,
                            std::string* mac) {
  return HmacSha256(key, message, mac);
}

}  // namespace http
}  // namespace zrpc

#else

namespace zrpc {
namespace http {

std::unique_ptr<SymmetricCipher> NewAesGcmCipher(const std::string& /*key*/) {
  return nullptr;
}

std::unique_ptr<AsymmetricCipher> NewRsaCipher(const std::string& /*public_pem*/,
                                                const std::string& /*private_pem*/) {
  return nullptr;
}

std::unique_ptr<PasswordHasher> NewPbkdf2Hasher(uint32_t /*iterations*/) {
  return nullptr;
}

CryptoStatus GenerateSecureRandom(size_t /*size*/, std::string* /*output*/) {
  return CryptoStatus::Failure("openssl backend unavailable");
}

}  // namespace http
}  // namespace zrpc

#endif
