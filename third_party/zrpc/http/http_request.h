#pragma once

#include <map>
#include <string>

#include "zrpc/base/buffer.h"

namespace zrpc {
class HttpResponse;
class HttpRequest {
 public:
  HttpRequest() : method_(kInvalid), version_(kUnknown), query_length_(0),
                  receive_time_(0), content_length_(-1), index_(0) {}

  enum Method { kInvalid, kGet, kPost, kHead, kPut, kDelete, kContent };
  enum Version { kUnknown, kHttp10, kHttp11 };

  void SetVersion(Version v) { version_ = v; }
  Version getVersion() const { return version_; }

  void SetMethod(Method method) { method_ = method; }
  void SetMethod();

  bool SetMethod(const char *start, const char *end);
  Method GetMethod() const { return method_; }

  const char *MethodString() const;
  const std::string &GetPath() const;

  bool SetPath(const char *start, const char *end);
  bool SetQuery(const std::string &query);
  bool SetQuery(const char *start, const char *end);
	
  void SetBody(const std::string &body) { body_ = body; }
  void SetBody(const char *start, const char *end);

  const std::string &GetBody() const;
  const std::string &GetQuery() const;

  void SetReceiveTime(int64_t t);
  bool AddHeader(const char *start, const char *colon, const char *end);
  bool AddHeader(const std::string &key, const std::string &value);

  std::string GetHeader(const std::string &field) const;
  size_t CountHeader(const std::string &field) const;
  const std::multimap<std::string, std::string> &GetHeaders() const;
  void Swap(HttpRequest &that);
  bool AppendToBuffer(Buffer *output) const;
  void SetIndex(int64_t idx) { index_ = idx; }
  int64_t GetIndex() const { return index_; }

  static bool IsValidHeaderName(const std::string &name);
  static bool IsValidHeaderValue(const std::string &value);
  static bool IsValidRequestTarget(const std::string &target);

 private:
  Method method_;
  Version version_;
  std::string path_;
  std::string query_;
  std::string body_;
  int32_t query_length_;
  std::multimap<std::string, std::string> headers_;
  int64_t receive_time_;
  int32_t content_length_;
  int64_t index_;
};
}  // namespace zrpc
