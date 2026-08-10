#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "zrpc/http/http_request.h"
#include "zrpc/http/http_response.h"
namespace zrpc {
class Buffer;

class HttpContext {
 public:
  enum HttpRequestParseState {
    kExpectRequestLine,
    kExpectHeaders,
    kExpectBody,
    kExpectBodyUntilEof,
    kGotAll,
    kError,
  };

  struct Limits {
    size_t max_start_line_bytes = 8192;
    size_t max_header_line_bytes = 8192;
    size_t max_header_bytes = 64 * 1024;
    size_t max_header_count = 100;
    size_t max_body_bytes = 8 * 1024 * 1024;
    size_t max_interim_responses = 8;
  };

  HttpContext();
  explicit HttpContext(const Limits &limits);

  bool ParseRequest(Buffer *buf);
  bool ParseResponse(Buffer *buf);
  bool ParseResponseEof(Buffer *buf);
  bool GotAll() const { return state_ == kGotAll; }
  bool HasError() const { return state_ == kError; }
  const std::string &GetError() const { return error_; }
  void SetResponseToHeadRequest(bool is_head) {
    response_to_head_request_ = is_head;
  }

  void Reset();

  HttpRequest &GetRequest() { return request_; }
  HttpResponse &GetResponse() { return response_; }
  bool ProcessRequestLine(const char *begin, const char *end);
  bool ProcessResponseLine(const char *begin, const char *end);

 private:
  bool FindLine(Buffer *buf, size_t line_limit, const char **crlf);
  bool ProcessHeaderLine(const char *begin, const char *end, bool response);
  bool FinishRequestHeaders();
  bool FinishResponseHeaders();
  bool Fail(const std::string &message);
  void ResetMessageState(bool preserve_interim_count);

  HttpRequestParseState state_;
  Limits limits_;
  bool content_length_seen_;
  bool transfer_encoding_seen_;
  bool host_seen_;
  bool response_to_head_request_;
  bool response_http10_;
  int64_t expected_body_size_;
  size_t scan_offset_;
  size_t header_bytes_;
  size_t header_count_;
  size_t interim_response_count_;
  std::unordered_set<std::string> seen_header_names_;
  std::string error_;
  HttpRequest request_;
  HttpResponse response_;
};
}  // namespace zrpc
