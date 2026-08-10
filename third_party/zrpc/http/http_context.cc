#include "zrpc/http/http_context.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>

#include "zrpc/base/buffer.h"

namespace zrpc {
namespace {
std::string AsciiLower(const std::string &value) {
  std::string lowered = value;
  for (char &ch : lowered) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return lowered;
}

bool EqualsIgnoreCase(const std::string &left, const std::string &right) {
  return AsciiLower(left) == AsciiLower(right);
}

bool ContainsToken(const std::string &value, const std::string &expected) {
  size_t begin = 0;
  while (begin < value.size()) {
    size_t end = value.find(',', begin);
    if (end == std::string::npos) {
      end = value.size();
    }
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) {
      ++begin;
    }
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t')) {
      --end;
    }
    if (EqualsIgnoreCase(value.substr(begin, end - begin), expected)) {
      return true;
    }
    begin = end + 1;
  }
  return false;
}

bool RejectDuplicateHeader(const std::string &normalized_name,
                           bool response) {
  if (normalized_name == "content-length" ||
      normalized_name == "transfer-encoding" ||
      normalized_name == "connection" ||
      normalized_name == "authorization" ||
      normalized_name == "proxy-authorization" ||
      normalized_name == "upgrade") {
    return true;
  }
  return !response &&
         (normalized_name == "host" || normalized_name == "expect");
}

bool HasInvalidLineByte(const char *begin, const char *end) {
  return std::any_of(begin, end, [](char ch) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    return byte < 0x20 || byte == 0x7f;
  });
}
}  // namespace

HttpContext::HttpContext() : HttpContext(Limits()) {}

HttpContext::HttpContext(const Limits &limits)
    : state_(kExpectRequestLine),
      limits_(limits),
      content_length_seen_(false),
      transfer_encoding_seen_(false),
      host_seen_(false),
      response_to_head_request_(false),
      response_http10_(false),
      expected_body_size_(0),
      scan_offset_(0),
      header_bytes_(0),
      header_count_(0),
      interim_response_count_(0) {}

void HttpContext::Reset() {
  state_ = kExpectRequestLine;
  content_length_seen_ = false;
  transfer_encoding_seen_ = false;
  host_seen_ = false;
  response_to_head_request_ = false;
  response_http10_ = false;
  expected_body_size_ = 0;
  scan_offset_ = 0;
  header_bytes_ = 0;
  header_count_ = 0;
  interim_response_count_ = 0;
  seen_header_names_.clear();
  error_.clear();
  HttpRequest request;
  request_.Swap(request);
  response_ = HttpResponse();
}

void HttpContext::ResetMessageState(bool preserve_interim_count) {
  state_ = kExpectRequestLine;
  content_length_seen_ = false;
  transfer_encoding_seen_ = false;
  host_seen_ = false;
  response_http10_ = false;
  expected_body_size_ = 0;
  scan_offset_ = 0;
  header_bytes_ = 0;
  header_count_ = 0;
  seen_header_names_.clear();
  error_.clear();
  response_ = HttpResponse();
  if (!preserve_interim_count) {
    interim_response_count_ = 0;
  }
}

bool HttpContext::Fail(const std::string &message) {
  state_ = kError;
  error_ = message;
  return false;
}

bool HttpContext::FindLine(Buffer *buf, size_t line_limit,
                           const char **crlf) {
  if (buf == nullptr || crlf == nullptr) {
    return Fail("null HTTP parse buffer");
  }
  const size_t readable = static_cast<size_t>(buf->ReadableBytes());
  size_t start_offset = std::min(scan_offset_, readable);
  if (start_offset > 0) {
    --start_offset;
  }
  *crlf = buf->FindCRLF(buf->Peek() + start_offset);
  if (*crlf != nullptr) {
    const size_t line_size = static_cast<size_t>(*crlf - buf->Peek());
    if (line_size > line_limit) {
      return Fail("HTTP line exceeds configured limit");
    }
    scan_offset_ = 0;
    return true;
  }
  if (readable > line_limit) {
    return Fail("incomplete HTTP line exceeds configured limit");
  }
  scan_offset_ = readable;
  return true;
}

bool HttpContext::ProcessHeaderLine(const char *begin, const char *end,
                                    bool response) {
  if (header_count_ >= limits_.max_header_count) {
    return Fail("HTTP header count exceeds configured limit");
  }
  const char *colon = std::find(begin, end, ':');
  if (colon == end || begin == colon) {
    return Fail("malformed HTTP header");
  }
  const std::string name(begin, colon);
  if (!HttpRequest::IsValidHeaderName(name)) {
    return Fail("invalid HTTP header name");
  }
  const std::string normalized_name = AsciiLower(name);
  const bool duplicate =
      !seen_header_names_.insert(normalized_name).second;
  if (duplicate && RejectDuplicateHeader(normalized_name, response)) {
    return Fail("duplicate HTTP header");
  }

  const char *value_begin = colon + 1;
  while (value_begin < end &&
         (*value_begin == ' ' || *value_begin == '\t')) {
    ++value_begin;
  }
  const char *value_end = end;
  while (value_end > value_begin &&
         (value_end[-1] == ' ' || value_end[-1] == '\t')) {
    --value_end;
  }
  const std::string value(value_begin, value_end);
  if (!HttpRequest::IsValidHeaderValue(value)) {
    return Fail("invalid HTTP header value");
  }

  if (normalized_name == "content-length") {
    if (value.empty()) {
      return Fail("empty Content-Length");
    }
    uint64_t parsed = 0;
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() ||
        result.ptr != value.data() + value.size() ||
        parsed > limits_.max_body_bytes ||
        parsed > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      return Fail("invalid or excessive Content-Length");
    }
    content_length_seen_ = true;
    expected_body_size_ = static_cast<int64_t>(parsed);
  } else if (normalized_name == "transfer-encoding") {
    transfer_encoding_seen_ = true;
  } else if (!response && normalized_name == "host") {
    if (value.empty()) {
      return Fail("empty Host header");
    }
    host_seen_ = true;
  }

  const bool added = response
                         ? response_.AddHeader(name, value)
                         : request_.AddHeader(name, value);
  if (!added) {
    return Fail("HTTP header could not be stored safely");
  }
  ++header_count_;
  return true;
}

bool HttpContext::FinishRequestHeaders() {
  if (content_length_seen_ && transfer_encoding_seen_) {
    return Fail("request contains both Transfer-Encoding and Content-Length");
  }
  if (transfer_encoding_seen_) {
    return Fail("Transfer-Encoding requests are not supported");
  }
  if (request_.getVersion() == HttpRequest::kHttp11 && !host_seen_) {
    return Fail("HTTP/1.1 request requires exactly one Host header");
  }
  state_ = expected_body_size_ > 0 ? kExpectBody : kGotAll;
  return true;
}

bool HttpContext::FinishResponseHeaders() {
  if (content_length_seen_ && transfer_encoding_seen_) {
    return Fail("response contains both Transfer-Encoding and Content-Length");
  }
  if (transfer_encoding_seen_) {
    return Fail("Transfer-Encoding responses are not supported");
  }

  const int status = static_cast<int>(response_.GetStatusCode());
  if (status >= 100 && status < 200) {
    if (status == 101) {
      return Fail("HTTP protocol switching is not supported");
    }
    if (content_length_seen_) {
      return Fail("interim response must not contain Content-Length");
    }
    ++interim_response_count_;
    if (interim_response_count_ > limits_.max_interim_responses) {
      return Fail("too many interim HTTP responses");
    }
    ResetMessageState(true);
    return true;
  }

  if (response_to_head_request_ || status == 204 || status == 304) {
    if (status == 204 && content_length_seen_) {
      return Fail("204 response must not contain Content-Length");
    }
    state_ = kGotAll;
    return true;
  }
  if (content_length_seen_) {
    state_ = expected_body_size_ > 0 ? kExpectBody : kGotAll;
    return true;
  }

  const std::string connection = response_.GetHeader("Connection");
  if (!response_http10_ && !ContainsToken(connection, "close")) {
    return Fail("response body has no safe framing");
  }
  state_ = kExpectBodyUntilEof;
  return true;
}

bool HttpContext::ProcessResponseLine(const char *begin, const char *end) {
  if (begin == nullptr || end == nullptr || end - begin < 12 ||
      HasInvalidLineByte(begin, end) ||
      !std::equal(begin, begin + 7, "HTTP/1.") ||
      (begin[7] != '0' && begin[7] != '1') || begin[8] != ' ') {
    return false;
  }
  response_http10_ = begin[7] == '0';
  const char *code_begin = begin + 9;
  const char *code_end = code_begin + 3;
  if (code_end > end || !response_.SetStatusCode(code_begin, code_end)) {
    return false;
  }
  if (code_end == end) {
    return response_.SetStatusMessage("");
  }
  return *code_end == ' ' &&
         response_.SetStatusMessage(code_end + 1, end);
}

bool HttpContext::ProcessRequestLine(const char *begin, const char *end) {
  if (begin == nullptr || end == nullptr || begin >= end ||
      HasInvalidLineByte(begin, end)) {
    return false;
  }
  const char *start = begin;
  const char *space = std::find(start, end, ' ');
  if (space == end || !request_.SetMethod(start, space)) {
    return false;
  }
  start = space + 1;
  space = std::find(start, end, ' ');
  if (space == end || start == space) {
    return false;
  }
  const std::string target(start, space);
  if (!HttpRequest::IsValidRequestTarget(target)) {
    return false;
  }
  const char *question = std::find(start, space, '?');
  if (question != space) {
    if (!request_.SetPath(start, question) ||
        !request_.SetQuery(question + 1, space)) {
      return false;
    }
  } else if (!request_.SetPath(start, space)) {
    return false;
  }

  start = space + 1;
  if (end - start != 8 || !std::equal(start, end - 1, "HTTP/1.")) {
    return false;
  }
  if (*(end - 1) == '1') {
    request_.SetVersion(HttpRequest::kHttp11);
    return true;
  }
  if (*(end - 1) == '0') {
    request_.SetVersion(HttpRequest::kHttp10);
    return true;
  }
  return false;
}

bool HttpContext::ParseResponse(Buffer *buf) {
  if (buf == nullptr || state_ == kError) {
    return false;
  }
  while (state_ != kGotAll && state_ != kError) {
    if (state_ == kExpectRequestLine) {
      const char *crlf = nullptr;
      if (!FindLine(buf, limits_.max_start_line_bytes, &crlf)) {
        return false;
      }
      if (crlf == nullptr) {
        return true;
      }
      if (!ProcessResponseLine(buf->Peek(), crlf)) {
        return Fail("invalid HTTP response status line");
      }
      buf->RetrieveUntil(crlf + 2);
      state_ = kExpectHeaders;
    } else if (state_ == kExpectHeaders) {
      const char *crlf = nullptr;
      if (!FindLine(buf, limits_.max_header_line_bytes, &crlf)) {
        return false;
      }
      if (crlf == nullptr) {
        if (header_bytes_ + static_cast<size_t>(buf->ReadableBytes()) >
            limits_.max_header_bytes) {
          return Fail("HTTP headers exceed configured total limit");
        }
        return true;
      }
      const size_t consumed = static_cast<size_t>(crlf - buf->Peek()) + 2;
      if (consumed > limits_.max_header_bytes ||
          header_bytes_ > limits_.max_header_bytes - consumed) {
        return Fail("HTTP headers exceed configured total limit");
      }
      header_bytes_ += consumed;
      if (crlf == buf->Peek()) {
        buf->RetrieveUntil(crlf + 2);
        if (!FinishResponseHeaders()) {
          return false;
        }
      } else {
        if (!ProcessHeaderLine(buf->Peek(), crlf, true)) {
          return false;
        }
        buf->RetrieveUntil(crlf + 2);
      }
    } else if (state_ == kExpectBody) {
      if (expected_body_size_ < 0 ||
          static_cast<uint64_t>(expected_body_size_) >
              limits_.max_body_bytes) {
        return Fail("invalid HTTP response body length");
      }
      if (static_cast<int64_t>(buf->ReadableBytes()) >=
          expected_body_size_) {
        response_.SetBody(
            std::string(buf->Peek(), static_cast<size_t>(expected_body_size_)));
        buf->Retrieve(static_cast<int32_t>(expected_body_size_));
        state_ = kGotAll;
      } else {
        return true;
      }
    } else if (state_ == kExpectBodyUntilEof) {
      const size_t readable = static_cast<size_t>(buf->ReadableBytes());
      if (readable > limits_.max_body_bytes ||
          response_.GetBody().size() >
              limits_.max_body_bytes - readable) {
        return Fail("close-delimited HTTP body exceeds configured limit");
      }
      response_.AppendBody(buf->Peek(), readable);
      buf->RetrieveAll();
      return true;
    } else {
      return Fail("invalid HTTP response parser state");
    }
  }
  return state_ != kError;
}

bool HttpContext::ParseRequest(Buffer *buf) {
  if (buf == nullptr || state_ == kError) {
    return false;
  }
  while (state_ != kGotAll && state_ != kError) {
    if (state_ == kExpectRequestLine) {
      const char *crlf = nullptr;
      if (!FindLine(buf, limits_.max_start_line_bytes, &crlf)) {
        return false;
      }
      if (crlf == nullptr) {
        return true;
      }
      if (!ProcessRequestLine(buf->Peek(), crlf)) {
        return Fail("invalid HTTP request line");
      }
      buf->RetrieveUntil(crlf + 2);
      state_ = kExpectHeaders;
    } else if (state_ == kExpectHeaders) {
      const char *crlf = nullptr;
      if (!FindLine(buf, limits_.max_header_line_bytes, &crlf)) {
        return false;
      }
      if (crlf == nullptr) {
        if (header_bytes_ + static_cast<size_t>(buf->ReadableBytes()) >
            limits_.max_header_bytes) {
          return Fail("HTTP headers exceed configured total limit");
        }
        return true;
      }
      const size_t consumed = static_cast<size_t>(crlf - buf->Peek()) + 2;
      if (consumed > limits_.max_header_bytes ||
          header_bytes_ > limits_.max_header_bytes - consumed) {
        return Fail("HTTP headers exceed configured total limit");
      }
      header_bytes_ += consumed;
      if (crlf == buf->Peek()) {
        buf->RetrieveUntil(crlf + 2);
        if (!FinishRequestHeaders()) {
          return false;
        }
      } else {
        if (!ProcessHeaderLine(buf->Peek(), crlf, false)) {
          return false;
        }
        buf->RetrieveUntil(crlf + 2);
      }
    } else if (state_ == kExpectBody) {
      if (expected_body_size_ <= 0 ||
          static_cast<uint64_t>(expected_body_size_) >
              limits_.max_body_bytes) {
        return Fail("invalid HTTP request body length");
      }
      if (static_cast<int64_t>(buf->ReadableBytes()) >=
          expected_body_size_) {
        request_.SetBody(buf->Peek(), buf->Peek() + expected_body_size_);
        buf->Retrieve(static_cast<int32_t>(expected_body_size_));
        state_ = kGotAll;
      } else {
        return true;
      }
    } else {
      return Fail("invalid HTTP request parser state");
    }
  }
  return state_ != kError;
}

bool HttpContext::ParseResponseEof(Buffer *buf) {
  if (!ParseResponse(buf)) {
    return false;
  }
  if (state_ == kExpectBodyUntilEof) {
    state_ = kGotAll;
    return true;
  }
  if (state_ == kGotAll) {
    return true;
  }
  return Fail("unexpected EOF in HTTP response");
}
}  // namespace zrpc
