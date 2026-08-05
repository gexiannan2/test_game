#include "zrpc/http/crypto.h"

#include <cctype>

namespace zrpc {
namespace http {
namespace {

std::string Base64Encode(const uint8_t* data, size_t len, bool url_safe) {
  if (data == nullptr && len != 0) {
    return {};
  }
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static const char kUrlTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  const char* table = url_safe ? kUrlTable : kTable;
  std::string out;
  out.reserve(((len + 2) / 3) * 4);

  size_t i = 0;
  while (i + 2 < len) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    out.push_back(table[(n >> 18) & 0x3F]);
    out.push_back(table[(n >> 12) & 0x3F]);
    out.push_back(table[(n >> 6) & 0x3F]);
    out.push_back(table[n & 0x3F]);
    i += 3;
  }

  if (i < len) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) {
      n |= static_cast<uint32_t>(data[i + 1]) << 8;
    }
    out.push_back(table[(n >> 18) & 0x3F]);
    out.push_back(table[(n >> 12) & 0x3F]);
    if (i + 1 < len) {
      out.push_back(table[(n >> 6) & 0x3F]);
      if (!url_safe) {
        out.push_back('=');
      }
    } else {
      if (!url_safe) {
        out.push_back('=');
        out.push_back('=');
      }
    }
  }
  return out;
}

int Base64Value(char ch, bool url_safe) {
  if (ch >= 'A' && ch <= 'Z') {
    return ch - 'A';
  }
  if (ch >= 'a' && ch <= 'z') {
    return ch - 'a' + 26;
  }
  if (ch >= '0' && ch <= '9') {
    return ch - '0' + 52;
  }
  if (!url_safe && ch == '+') {
    return 62;
  }
  if (!url_safe && ch == '/') {
    return 63;
  }
  if (url_safe && ch == '-') {
    return 62;
  }
  if (url_safe && ch == '_') {
    return 63;
  }
  return -1;
}

bool Base64DecodeImpl(const std::string& input, bool url_safe, std::string* output) {
  if (output == nullptr) {
    return false;
  }

  size_t padding = 0;
  while (padding < input.size() && input[input.size() - padding - 1] == '=') {
    ++padding;
  }
  if (padding > 2) {
    return false;
  }
  const size_t encoded_size = input.size() - padding;
  if ((padding != 0 && input.size() % 4 != 0) ||
      (padding == 1 && encoded_size % 4 != 3) ||
      (padding == 2 && encoded_size % 4 != 2) ||
      (padding == 0 && encoded_size % 4 == 1)) {
    return false;
  }

  std::vector<uint8_t> bytes;
  bytes.reserve((encoded_size * 6) / 8);

  uint32_t value = 0;
  int bits = 0;
  for (size_t i = 0; i < encoded_size; ++i) {
    const char ch = input[i];
    const int idx = Base64Value(ch, url_safe);
    if (idx < 0) {
      return false;
    }
    value = (value << 6) | static_cast<uint32_t>(idx);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(static_cast<uint8_t>((value >> bits) & 0xFF));
      value &= bits == 0 ? 0u : ((1u << bits) - 1u);
    }
  }

  if (value != 0) {
    return false;
  }
  if (bytes.empty()) {
    output->clear();
  } else {
    output->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return true;
}

}  // namespace

std::string Base64UrlEncode(const std::string& input) {
  return Base64UrlEncode(reinterpret_cast<const uint8_t*>(input.data()),
                         input.size());
}

std::string Base64UrlEncode(const uint8_t* data, size_t len) {
  return Base64Encode(data, len, true);
}

bool Base64UrlDecode(const std::string& input, std::string* output) {
  return Base64DecodeImpl(input, true, output);
}

bool CryptoBackendAvailable() {
#ifdef ZRPC_USE_OPENSSL
  return true;
#else
  return false;
#endif
}

#ifndef ZRPC_USE_OPENSSL
CryptoStatus HmacSha256Sign(const std::string& /*key*/, const std::string& /*message*/,
                            std::string* /*mac*/) {
  return CryptoStatus::Failure("openssl backend unavailable");
}
#endif

std::unique_ptr<SymmetricCipher> NewAesGcmCipher(const std::string& key);
std::unique_ptr<AsymmetricCipher> NewRsaCipher(const std::string& public_pem,
                                                const std::string& private_pem);
std::unique_ptr<PasswordHasher> NewPbkdf2Hasher(uint32_t iterations);

}  // namespace http
}  // namespace zrpc
