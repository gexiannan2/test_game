#pragma once

#include <map>
#include <string>

namespace zrpc {
class Buffer;

class HttpResponse {
 public:
  enum HttpStatusCode {
    kUnknown,
    k200k = 200,
    k301MovedPermanently = 301,
    k400BadRequest = 400,
    k401Unauthorized = 401,
    k408RequestTimeout = 408,
    k403Forbidden = 403,
    k404NotFound = 404,
    k500InternalServerError = 500,
    k502BadGateway = 502,
    k504GatewayTimeout = 504,
  };

  explicit HttpResponse()
      : status_code_(kUnknown),
        close_connection_(false),
        suppress_body_(false) {}

  bool SetStatusCode(const char *start, const char *end);

  bool SetStatusMessage(const char *start, const char *end);

  HttpStatusCode GetStatusCode() const { return status_code_; }
  bool SetStatusCode(HttpStatusCode code);
  bool SetStatusCode(int code);

  bool SetStatusMessage(const std::string &message);

  void SetCloseConnection(bool on) { close_connection_ = on; }
  bool GetCloseConnection() const { return close_connection_; }

  void SetContentType(const std::string &contentType) {
    AddHeader("Content-Type", contentType);
  }

  bool AddHeader(const char *start, const char *colon, const char *end);
  bool AddHeader(const std::string &key, const std::string &value);
  std::string GetHeader(const std::string &field) const;
  size_t CountHeader(const std::string &field) const;
  const std::multimap<std::string, std::string> &GetHeaders() const {
    return headers_;
  }

  void SetBody(const std::string &body) { body_ = body; }
  void AppendBody(const char *data, size_t len) { body_.append(data, len); }
  const std::string &GetBody() const { return body_; }
  void SetSuppressBody(bool suppress) { suppress_body_ = suppress; }
  bool GetSuppressBody() const { return suppress_body_; }
  bool AppendToBuffer(Buffer *output) const;
  int32_t GetBodySize() const;

  static bool StatusAllowsBody(int code);

 private:
  std::multimap<std::string, std::string> headers_;
  HttpStatusCode status_code_;
  std::string status_message_;
  bool close_connection_;
  bool suppress_body_;
  std::string body_;
};
}  // namespace zrpc
