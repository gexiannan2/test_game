#include "zrpc/http/http_response.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>

#include "zrpc/base/buffer.h"
#include "zrpc/http/http_request.h"

namespace zrpc {
namespace {

bool HeaderNameEquals(const std::string &left, const std::string &right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

bool IsValidReasonPhrase(const std::string &message) {
  return std::none_of(message.begin(), message.end(), [](char ch) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    return byte == '\r' || byte == '\n' || byte == 0 ||
           (byte < 0x20 && byte != '\t') || byte == 0x7f;
  });
}

}  // namespace

bool HttpResponse::SetStatusCode(const char *start, const char *end) {
  if (start == nullptr || end == nullptr || end - start != 3) {
    return false;
  }
  int code = 0;
  const auto result = std::from_chars(start, end, code);
  return result.ec == std::errc() && result.ptr == end &&
         SetStatusCode(code);
}

bool HttpResponse::SetStatusCode(HttpStatusCode code) {
  return SetStatusCode(static_cast<int>(code));
}

bool HttpResponse::SetStatusCode(int code) {
  if (code < 100 || code > 599) {
    return false;
  }
  status_code_ = static_cast<HttpStatusCode>(code);
  return true;
}

bool HttpResponse::SetStatusMessage(const char *start, const char *end) {
  if (start == nullptr || end == nullptr || start > end) {
    return false;
  }
  return SetStatusMessage(std::string(start, end));
}

bool HttpResponse::SetStatusMessage(const std::string &message) {
  if (!IsValidReasonPhrase(message)) {
    return false;
  }
  status_message_ = message;
  return true;
}

bool HttpResponse::AddHeader(const char *start, const char *colon,
                             const char *end) {
  if (start == nullptr || colon == nullptr || end == nullptr ||
      start >= colon || colon >= end) {
    return false;
  }
  std::string field(start, colon);
  ++colon;
  while (colon < end && (*colon == ' ' || *colon == '\t')) {
    ++colon;
  }
  std::string value(colon, end);
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  return AddHeader(field, value);
}

bool HttpResponse::AddHeader(const std::string &key,
                             const std::string &value) {
  if (!HttpRequest::IsValidHeaderName(key) ||
      !HttpRequest::IsValidHeaderValue(value)) {
    return false;
  }
  headers_.emplace(key, value);
  return true;
}

std::string HttpResponse::GetHeader(const std::string &field) const {
  for (const auto &header : headers_) {
    if (HeaderNameEquals(header.first, field)) {
      return header.second;
    }
  }
  return {};
}

size_t HttpResponse::CountHeader(const std::string &field) const {
  size_t count = 0;
  for (const auto &header : headers_) {
    if (HeaderNameEquals(header.first, field)) {
      ++count;
    }
  }
  return count;
}

int32_t HttpResponse::GetBodySize() const {
  bool found = false;
  int32_t body_size = -1;
  for (const auto &header : headers_) {
    if (HeaderNameEquals(header.first, "Content-Length")) {
      if (header.second.empty()) {
        return -2;
      }
      uint64_t value = 0;
      const char *begin = header.second.data();
      const char *end = begin + header.second.size();
      const auto result = std::from_chars(begin, end, value);
      if (result.ec != std::errc() || result.ptr != end ||
          value > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        return -2;
      }
      const int32_t current = static_cast<int32_t>(value);
      if (found && current != body_size) {
        return -2;
      }
      found = true;
      body_size = current;
    }
  }
  return body_size;
}

bool HttpResponse::StatusAllowsBody(int code) {
  return !((code >= 100 && code < 200) || code == 204 || code == 304);
}

bool HttpResponse::AppendToBuffer(Buffer *output) const {
  const int status_code = static_cast<int>(status_code_);
  if (output == nullptr || status_code < 100 || status_code > 599 ||
      !IsValidReasonPhrase(status_message_) ||
      CountHeader("Transfer-Encoding") != 0) {
    return false;
  }

  Buffer serialized;
  char buf[32] = {};
  snprintf(buf, sizeof buf, "HTTP/1.1 %d ", status_code);
  serialized.Append(buf);
  serialized.Append(status_message_);
  serialized.Append("\r\n");

  const bool status_allows_body = StatusAllowsBody(status_code);
  if (status_allows_body) {
    snprintf(buf, sizeof buf, "Content-Length: %zu\r\n", body_.size());
    serialized.Append(buf);
  }
  serialized.Append(close_connection_ ? "Connection: close\r\n"
                                      : "Connection: keep-alive\r\n");

  size_t header_bytes = 0;
  for (const auto &header : headers_) {
    if (!HttpRequest::IsValidHeaderName(header.first) ||
        !HttpRequest::IsValidHeaderValue(header.second)) {
      return false;
    }
    if (HeaderNameEquals(header.first, "Content-Length") ||
        HeaderNameEquals(header.first, "Connection")) {
      continue;
    }
    if (HeaderNameEquals(header.first, "Transfer-Encoding")) {
      return false;
    }
    header_bytes += header.first.size() + header.second.size() + 4;
    if (header_bytes > 64 * 1024) {
      return false;
    }
    serialized.Append(header.first);
    serialized.Append(": ");
    serialized.Append(header.second);
    serialized.Append("\r\n");
  }

  serialized.Append("\r\n");
  if (status_allows_body && !suppress_body_ && !body_.empty()) {
    serialized.Append(body_.c_str(), body_.size());
  }
  output->Append(serialized.ToStringView());
  return true;
}
}  // namespace zrpc
