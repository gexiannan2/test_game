#include "zrpc/http/jwt.h"

#include <charconv>
#include <chrono>
#include <limits>
#include <set>
#include <sstream>

namespace zrpc {
namespace http {
namespace {

constexpr size_t kMaxJwtBytes = 16 * 1024;
constexpr size_t kMaxJwtHeaderBytes = 1024;
constexpr size_t kMaxJwtPayloadBytes = 12 * 1024;

int64_t NowUnix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string JsonEscape(const std::string& input) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(input.size() + 8);
  for (unsigned char ch : input) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(ch >> 4) & 0x0F]);
          out.push_back(kHex[ch & 0x0F]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return out;
}

std::string ClaimsToJson(const JwtClaims& claims) {
  std::ostringstream oss;
  oss << "{";
  bool first = true;
  auto append_field = [&](const std::string& key, const std::string& value) {
    if (value.empty()) {
      return;
    }
    if (!first) {
      oss << ",";
    }
    first = false;
    oss << "\"" << JsonEscape(key) << "\":\"" << JsonEscape(value) << "\"";
  };
  auto append_number = [&](const std::string& key, int64_t value) {
    if (value == 0) {
      return;
    }
    if (!first) {
      oss << ",";
    }
    first = false;
    oss << "\"" << key << "\":" << value;
  };

  append_field("sub", claims.sub);
  append_field("iss", claims.iss);
  append_field("aud", claims.aud);
  append_number("exp", claims.exp);
  append_number("iat", claims.iat);
  append_number("nbf", claims.nbf);
  for (const auto& item : claims.custom) {
    if (item.first == "sub" || item.first == "iss" || item.first == "aud" ||
        item.first == "exp" || item.first == "iat" || item.first == "nbf") {
      continue;
    }
    append_field(item.first, item.second);
  }
  oss << "}";
  return oss.str();
}

void SkipSpaces(const std::string& json, size_t* pos) {
  while (*pos < json.size() &&
         std::isspace(static_cast<unsigned char>(json[*pos]))) {
    ++(*pos);
  }
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

bool ParseHexCodeUnit(const std::string& json, size_t start, uint32_t* value) {
  if (start + 4 > json.size()) {
    return false;
  }
  uint32_t parsed = 0;
  for (size_t i = 0; i < 4; ++i) {
    const int digit = HexValue(json[start + i]);
    if (digit < 0) {
      return false;
    }
    parsed = (parsed << 4) | static_cast<uint32_t>(digit);
  }
  *value = parsed;
  return true;
}

bool AppendUtf8(uint32_t codepoint, std::string* output) {
  if (codepoint <= 0x7F) {
    output->push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    output->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    output->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0x10FFFF) {
    output->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    output->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    return false;
  }
  return true;
}

bool ParseJsonString(const std::string& json, size_t* pos, std::string* out) {
  if (*pos >= json.size() || json[*pos] != '"') {
    return false;
  }
  ++(*pos);
  std::string value;
  while (*pos < json.size()) {
    const unsigned char ch = static_cast<unsigned char>(json[*pos]);
    if (ch == '"') {
      ++(*pos);
      *out = value;
      return true;
    }
    if (ch == '\\') {
      if (*pos + 1 >= json.size()) {
        return false;
      }
      const char escaped = json[*pos + 1];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          value.push_back(escaped);
          break;
        case 'b':
          value.push_back('\b');
          break;
        case 'f':
          value.push_back('\f');
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        case 'u': {
          uint32_t codepoint = 0;
          if (!ParseHexCodeUnit(json, *pos + 2, &codepoint)) {
            return false;
          }
          size_t consumed = 6;
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (*pos + 12 > json.size() || json[*pos + 6] != '\\' ||
                json[*pos + 7] != 'u') {
              return false;
            }
            uint32_t low = 0;
            if (!ParseHexCodeUnit(json, *pos + 8, &low) ||
                low < 0xDC00 || low > 0xDFFF) {
              return false;
            }
            codepoint =
                0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
            consumed = 12;
          } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return false;
          }
          if (!AppendUtf8(codepoint, &value)) {
            return false;
          }
          *pos += consumed;
          continue;
        }
        default:
          return false;
      }
      *pos += 2;
      continue;
    }
    if (ch < 0x20) {
      return false;
    }
    value.push_back(static_cast<char>(ch));
    ++(*pos);
  }
  return false;
}

bool ParseJsonNumber(const std::string& json, size_t* pos, int64_t* out) {
  const size_t start = *pos;
  if (*pos < json.size() && json[*pos] == '-') {
    ++(*pos);
  }
  const size_t digits_start = *pos;
  while (*pos < json.size() &&
         std::isdigit(static_cast<unsigned char>(json[*pos]))) {
    ++(*pos);
  }
  if (digits_start == *pos) {
    return false;
  }
  if (*pos - digits_start > 1 && json[digits_start] == '0') {
    return false;
  }
  const char* begin = json.data() + start;
  const char* end = json.data() + *pos;
  const auto result = std::from_chars(begin, end, *out);
  return result.ec == std::errc() && result.ptr == end;
}

bool JsonToClaims(const std::string& json, JwtClaims* claims) {
  if (claims == nullptr) {
    return false;
  }
  size_t pos = 0;
  SkipSpaces(json, &pos);
  if (pos >= json.size() || json[pos] != '{') {
    return false;
  }
  ++pos;
  std::set<std::string> seen_keys;

  while (pos < json.size()) {
    SkipSpaces(json, &pos);
    if (pos < json.size() && json[pos] == '}') {
      ++pos;
      SkipSpaces(json, &pos);
      return pos == json.size();
    }

    std::string key;
    if (!ParseJsonString(json, &pos, &key)) {
      return false;
    }
    if (!seen_keys.insert(key).second) {
      return false;
    }
    SkipSpaces(json, &pos);
    if (pos >= json.size() || json[pos] != ':') {
      return false;
    }
    ++pos;
    SkipSpaces(json, &pos);

    if (pos < json.size() && json[pos] == '"') {
      std::string value;
      if (!ParseJsonString(json, &pos, &value)) {
        return false;
      }
      if (key == "sub") {
        claims->sub = value;
      } else if (key == "iss") {
        claims->iss = value;
      } else if (key == "aud") {
        claims->aud = value;
      } else if (key == "exp" || key == "iat" || key == "nbf") {
        return false;
      } else {
        claims->custom[key] = value;
      }
    } else {
      int64_t number = 0;
      if (!ParseJsonNumber(json, &pos, &number)) {
        return false;
      }
      if (key == "exp") {
        claims->exp = number;
      } else if (key == "iat") {
        claims->iat = number;
      } else if (key == "nbf") {
        claims->nbf = number;
      } else if (key == "sub" || key == "iss" || key == "aud") {
        return false;
      } else {
        claims->custom[key] = std::to_string(number);
      }
    }

    SkipSpaces(json, &pos);
    if (pos < json.size() && json[pos] == ',') {
      ++pos;
      SkipSpaces(json, &pos);
      if (pos >= json.size() || json[pos] == '}') {
        return false;
      }
      continue;
    }
    if (pos < json.size() && json[pos] == '}') {
      ++pos;
      SkipSpaces(json, &pos);
      return pos == json.size();
    }
    return false;
  }
  return false;
}

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

bool SplitToken(const std::string& token, std::string* header, std::string* payload,
                std::string* signature) {
  const size_t first = token.find('.');
  if (first == std::string::npos) {
    return false;
  }
  const size_t second = token.find('.', first + 1);
  if (second == std::string::npos ||
      token.find('.', second + 1) != std::string::npos ||
      token.find('=') != std::string::npos) {
    return false;
  }
  *header = token.substr(0, first);
  *payload = token.substr(first + 1, second - first - 1);
  *signature = token.substr(second + 1);
  return !header->empty() && !payload->empty() && !signature->empty();
}

int64_t SaturatingAdd(int64_t value, int64_t delta) {
  if (delta > 0 && value > std::numeric_limits<int64_t>::max() - delta) {
    return std::numeric_limits<int64_t>::max();
  }
  return value + delta;
}

CryptoStatus ValidateClaims(const JwtClaims& claims, int64_t now,
                            const JwtValidationOptions& options) {
  if (options.clock_skew_seconds < 0 ||
      options.max_token_lifetime_seconds < 0) {
    return CryptoStatus::Failure("invalid jwt validation options");
  }
  if (options.require_expiration && claims.exp <= 0) {
    return CryptoStatus::Failure("jwt missing expiration");
  }
  if (options.require_issued_at && claims.iat <= 0) {
    return CryptoStatus::Failure("jwt missing issued-at time");
  }
  const int64_t latest_acceptable =
      SaturatingAdd(now, options.clock_skew_seconds);
  if (claims.iat > latest_acceptable) {
    return CryptoStatus::Failure("jwt issued in the future");
  }
  if (claims.nbf > latest_acceptable) {
    return CryptoStatus::Failure("jwt not active yet");
  }
  if (claims.exp > 0 &&
      SaturatingAdd(claims.exp, options.clock_skew_seconds) <= now) {
    return CryptoStatus::Failure("jwt expired");
  }
  if (claims.exp > 0 && claims.iat > 0) {
    if (claims.exp <= claims.iat) {
      return CryptoStatus::Failure("jwt expiration precedes issued-at time");
    }
    if (options.max_token_lifetime_seconds > 0 &&
        claims.exp - claims.iat > options.max_token_lifetime_seconds) {
      return CryptoStatus::Failure("jwt lifetime exceeds configured maximum");
    }
  }
  if (!options.expected_issuer.empty() &&
      claims.iss != options.expected_issuer) {
    return CryptoStatus::Failure("jwt issuer mismatch");
  }
  if (!options.expected_audience.empty() &&
      claims.aud != options.expected_audience) {
    return CryptoStatus::Failure("jwt audience mismatch");
  }
  return CryptoStatus::Success();
}

}  // namespace

bool JwtClaims::IsExpired(int64_t now_unix) const {
  return exp <= 0 || now_unix >= exp;
}

std::string JwtClaims::Get(const std::string& key) const {
  if (key == "sub") {
    return sub;
  }
  if (key == "iss") {
    return iss;
  }
  if (key == "aud") {
    return aud;
  }
  const auto it = custom.find(key);
  return it == custom.end() ? std::string() : it->second;
}

void JwtClaims::Set(const std::string& key, const std::string& value) {
  custom[key] = value;
}

Hs256Signer::Hs256Signer(std::string secret) : secret_(std::move(secret)) {}

std::string Hs256Signer::Algorithm() const { return "HS256"; }

CryptoStatus Hs256Signer::Sign(const std::string& signing_input,
                               std::string* signature) {
  if (signature == nullptr) {
    return CryptoStatus::Failure("null signature output");
  }
  if (secret_.size() < kMinHmacSha256KeyBytes) {
    return CryptoStatus::Failure("hmac secret must contain at least 32 bytes");
  }
  if (!CryptoBackendAvailable()) {
    return CryptoStatus::Failure("openssl backend unavailable");
  }
  std::string mac;
  const CryptoStatus status = HmacSha256Sign(secret_, signing_input, &mac);
  if (!status) {
    return status;
  }
  *signature = Base64UrlEncode(mac);
  return CryptoStatus::Success();
}

CryptoStatus Hs256Signer::Verify(const std::string& signing_input,
                                 const std::string& signature) {
  std::string expected;
  const CryptoStatus status = Sign(signing_input, &expected);
  if (!status) {
    return status;
  }
  if (!SecureEqual(expected, signature)) {
    return CryptoStatus::Failure("jwt signature mismatch");
  }
  return CryptoStatus::Success();
}

Rs256Signer::Rs256Signer(std::shared_ptr<AsymmetricCipher> rsa)
    : rsa_(std::move(rsa)) {}

std::string Rs256Signer::Algorithm() const { return "RS256"; }

CryptoStatus Rs256Signer::Sign(const std::string& signing_input,
                               std::string* signature) {
  if (rsa_ == nullptr || signature == nullptr) {
    return CryptoStatus::Failure("rsa signer unavailable");
  }
  std::string raw;
  const CryptoStatus status = rsa_->Sign(signing_input, &raw);
  if (!status) {
    return status;
  }
  *signature = Base64UrlEncode(raw);
  return CryptoStatus::Success();
}

CryptoStatus Rs256Signer::Verify(const std::string& signing_input,
                                 const std::string& signature) {
  if (rsa_ == nullptr) {
    return CryptoStatus::Failure("rsa signer unavailable");
  }
  std::string raw;
  if (!Base64UrlDecode(signature, &raw)) {
    return CryptoStatus::Failure("invalid jwt signature encoding");
  }
  return rsa_->Verify(signing_input, raw);
}

JwtCodec::JwtCodec(std::shared_ptr<JwtSigner> signer,
                   JwtValidationOptions options)
    : signer_(std::move(signer)), options_(std::move(options)) {}

CryptoStatus JwtCodec::Encode(const JwtClaims& claims, std::string* token,
                              int64_t now_unix) const {
  if (token == nullptr || signer_ == nullptr) {
    return CryptoStatus::Failure("invalid jwt encode arguments");
  }

  const int64_t now = now_unix > 0 ? now_unix : NowUnix();
  JwtClaims encoded = claims;
  if (encoded.iat == 0) {
    encoded.iat = now;
  }
  const CryptoStatus claims_status =
      ValidateClaims(encoded, now, options_);
  if (!claims_status) {
    return claims_status;
  }

  const std::string header =
      "{\"alg\":\"" + signer_->Algorithm() + "\",\"typ\":\"JWT\"}";
  const std::string payload = ClaimsToJson(encoded);
  if (header.size() > kMaxJwtHeaderBytes ||
      payload.size() > kMaxJwtPayloadBytes) {
    return CryptoStatus::Failure("jwt claims exceed configured size");
  }
  const std::string signing_input =
      Base64UrlEncode(header) + "." + Base64UrlEncode(payload);

  std::string signature;
  const CryptoStatus status = signer_->Sign(signing_input, &signature);
  if (!status) {
    return status;
  }
  *token = signing_input + "." + signature;
  if (token->size() > kMaxJwtBytes) {
    token->clear();
    return CryptoStatus::Failure("encoded jwt exceeds configured size");
  }
  return CryptoStatus::Success();
}

CryptoStatus JwtCodec::Decode(const std::string& token, JwtClaims* claims,
                              int64_t now_unix) const {
  if (claims == nullptr || signer_ == nullptr) {
    return CryptoStatus::Failure("invalid jwt decode arguments");
  }
  if (token.empty() || token.size() > kMaxJwtBytes) {
    return CryptoStatus::Failure("jwt exceeds configured size");
  }

  std::string header_b64;
  std::string payload_b64;
  std::string signature_b64;
  if (!SplitToken(token, &header_b64, &payload_b64, &signature_b64)) {
    return CryptoStatus::Failure("invalid jwt format");
  }

  const std::string signing_input = header_b64 + "." + payload_b64;
  const CryptoStatus verify_status =
      signer_->Verify(signing_input, signature_b64);
  if (!verify_status) {
    return verify_status;
  }

  std::string header_json;
  if (!Base64UrlDecode(header_b64, &header_json)) {
    return CryptoStatus::Failure("invalid jwt header encoding");
  }
  if (header_json.size() > kMaxJwtHeaderBytes) {
    return CryptoStatus::Failure("jwt header exceeds configured size");
  }
  JwtClaims header_claims;
  if (!JsonToClaims(header_json, &header_claims) ||
      header_claims.Get("alg") != signer_->Algorithm() ||
      header_claims.Get("typ") != "JWT") {
    return CryptoStatus::Failure("invalid jwt algorithm");
  }

  std::string payload_json;
  if (!Base64UrlDecode(payload_b64, &payload_json)) {
    return CryptoStatus::Failure("invalid jwt payload encoding");
  }
  if (payload_json.size() > kMaxJwtPayloadBytes) {
    return CryptoStatus::Failure("jwt payload exceeds configured size");
  }
  JwtClaims decoded_claims;
  if (!JsonToClaims(payload_json, &decoded_claims)) {
    return CryptoStatus::Failure("invalid jwt payload json");
  }

  const int64_t now = now_unix > 0 ? now_unix : NowUnix();
  const CryptoStatus claims_status =
      ValidateClaims(decoded_claims, now, options_);
  if (!claims_status) {
    return claims_status;
  }
  *claims = std::move(decoded_claims);
  return CryptoStatus::Success();
}

std::shared_ptr<JwtSigner> NewHs256Signer(const std::string& secret) {
  if (secret.size() < kMinHmacSha256KeyBytes) {
    return nullptr;
  }
  return std::make_shared<Hs256Signer>(secret);
}

std::shared_ptr<JwtSigner> NewRs256Signer(
    std::shared_ptr<AsymmetricCipher> rsa) {
  if (rsa == nullptr || rsa->Name() != "RSA") {
    return nullptr;
  }
  return std::make_shared<Rs256Signer>(std::move(rsa));
}

std::shared_ptr<JwtSigner> NewRs256Signer(
    std::unique_ptr<AsymmetricCipher> rsa) {
  return NewRs256Signer(std::shared_ptr<AsymmetricCipher>(std::move(rsa)));
}

}  // namespace http
}  // namespace zrpc
