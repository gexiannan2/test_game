#include "zrpc/http/http_request.h"

#include <algorithm>
#include <cctype>

namespace {

bool HeaderNameEquals(const std::string &left, const std::string &right)
{
  if (left.size() != right.size())
  {
    return false;
  }
  for (size_t i = 0; i < left.size(); ++i)
  {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i])))
    {
      return false;
    }
  }
  return true;
}

bool IsHeaderNameChar(unsigned char ch)
{
  if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
      (ch >= '0' && ch <= '9'))
  {
    return true;
  }
  switch (ch)
  {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

bool ContainsInvalidFieldValueByte(const std::string &value)
{
  return std::any_of(value.begin(), value.end(), [](char ch) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    return (byte < 0x20 && byte != '\t') || byte == 0x7f;
  });
}

bool IsSingletonRequestHeader(const std::string &name)
{
  return HeaderNameEquals(name, "Host") ||
         HeaderNameEquals(name, "Content-Length") ||
         HeaderNameEquals(name, "Transfer-Encoding") ||
         HeaderNameEquals(name, "Connection") ||
         HeaderNameEquals(name, "Authorization") ||
         HeaderNameEquals(name, "Proxy-Authorization") ||
         HeaderNameEquals(name, "Upgrade") ||
         HeaderNameEquals(name, "Expect");
}

}

namespace zrpc
{
  void HttpRequest::SetMethod() { method_ = kContent; }

  bool HttpRequest::SetMethod(const char *start, const char *end)
  {
    assert(method_ == kInvalid);
    std::string m(start, end);
    if (m == "GET")
    {
      method_ = kGet;
    }
    else if (m == "POST")
    {
      method_ = kPost;
    }
    else if (m == "HEAD")
    {
      method_ = kHead;
    }
    else if (m == "PUT")
    {
      method_ = kPut;
    }
    else if (m == "DELETE")
    {
      method_ = kDelete;
    }
    else
    {
      method_ = kInvalid;
    }
    return method_ != kInvalid;
  }

  const char *HttpRequest::MethodString() const
  {
    const char *result = "UNKNOWN";
    switch (method_)
    {
    case kGet:
    {
      result = "GET";
      break;
    }
    case kPost:
    {
      result = "POST";
      break;
    }
    case kHead:
    {
      result = "HEAD";
      break;
    }
    case kPut:
    {
      result = "PUT";
      break;
    }
    case kDelete:
    {
      result = "DELETE";
      break;
    }
    case kContent:
    {
      result = "CONTENT";
      break;
    }
    default:
      break;
    }
    return result;
  }

  const std::string &HttpRequest::GetPath() const { return path_; }

  bool HttpRequest::SetPath(const char *start, const char *end)
  {
    if (start == nullptr || end == nullptr || start >= end)
    {
      return false;
    }
    const std::string value(start, end);
    if (!IsValidRequestTarget(value) || value[0] != '/' ||
        value.find('?') != std::string::npos)
    {
      return false;
    }
    path_.assign(start, end);
    return true;
  }

  bool HttpRequest::SetQuery(const std::string &query)
  {
    return SetQuery(query.data(), query.data() + query.size());
  }

  bool HttpRequest::SetQuery(const char *start, const char *end)
  {
    if (start == nullptr || end == nullptr || start > end)
    {
      return false;
    }
    const std::string value(start, end);
    if (ContainsInvalidFieldValueByte(value) ||
        std::any_of(value.begin(), value.end(), [](char ch) {
          return ch == ' ' || ch == '\r' || ch == '\n' || ch == '#';
        }))
    {
      return false;
    }
    query_ = value;
    return true;
  }

  void HttpRequest::SetBody(const char *start, const char *end)
  {
    body_.assign(start, end);
  }

  const std::string &HttpRequest::GetBody() const { return body_; }
  const std::string &HttpRequest::GetQuery() const { return query_; }

  void HttpRequest::SetReceiveTime(int64_t t) { receive_time_ = t; }

  bool HttpRequest::IsValidHeaderName(const std::string &name)
  {
    return !name.empty() &&
           std::all_of(name.begin(), name.end(), [](char ch) {
             return IsHeaderNameChar(static_cast<unsigned char>(ch));
           });
  }

  bool HttpRequest::IsValidHeaderValue(const std::string &value)
  {
    return !ContainsInvalidFieldValueByte(value);
  }

  bool HttpRequest::IsValidRequestTarget(const std::string &target)
  {
    if (target.empty() || (target[0] != '/' && target != "*"))
    {
      return false;
    }
    return std::none_of(target.begin(), target.end(), [](char ch) {
      const unsigned char byte = static_cast<unsigned char>(ch);
      return byte <= 0x20 || byte == 0x7f || ch == '#';
    });
  }

  bool HttpRequest::AddHeader(const char *start, const char *colon,
                              const char *end)
  {
    if (start == nullptr || colon == nullptr || end == nullptr ||
        start >= colon || colon >= end)
    {
      return false;
    }
    std::string field(start, colon);
    ++colon;
    while (colon < end && (*colon == ' ' || *colon == '\t'))
    {
      ++colon;
    }

    std::string value(colon, end);
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t'))
    {
      value.resize(value.size() - 1);
    }
    return AddHeader(field, value);
  }

  std::string HttpRequest::GetHeader(const std::string &field) const
  {
    for (const auto &header : headers_)
    {
      if (HeaderNameEquals(header.first, field))
      {
        return header.second;
      }
    }
    return {};
  }

  size_t HttpRequest::CountHeader(const std::string &field) const
  {
    size_t count = 0;
    for (const auto &header : headers_)
    {
      if (HeaderNameEquals(header.first, field))
      {
        ++count;
      }
    }
    return count;
  }

  const std::multimap<std::string, std::string> &HttpRequest::GetHeaders() const
  {
    return headers_;
  }

  void HttpRequest::Swap(HttpRequest &that)
  {
    std::swap(method_, that.method_);
    std::swap(version_, that.version_);
    path_.swap(that.path_);
    query_.swap(that.query_);
    body_.swap(that.body_);
    std::swap(query_length_, that.query_length_);
    headers_.swap(that.headers_);
    std::swap(receive_time_, that.receive_time_);
    std::swap(content_length_, that.content_length_);
    std::swap(index_, that.index_);
  }

  bool HttpRequest::AddHeader(const std::string &key,
                              const std::string &value)
  {
    if (!IsValidHeaderName(key) || !IsValidHeaderValue(value) ||
        (IsSingletonRequestHeader(key) && CountHeader(key) != 0))
    {
      return false;
    }
    headers_.emplace(key, value);
    return true;
  }

  bool HttpRequest::AppendToBuffer(Buffer *output) const
  {
    if (output == nullptr || method_ == kInvalid || method_ == kContent ||
        CountHeader("Host") != 1 || GetHeader("Host").empty() ||
        CountHeader("Transfer-Encoding") != 0 ||
        (method_ == kHead && !body_.empty()))
    {
      return false;
    }

    std::string target;
    if (!path_.empty())
    {
      target = path_;
      if (!query_.empty())
      {
        if (query_[0] != '?')
        {
          target.push_back('?');
        }
        target.append(query_);
      }
    }
    else if (!query_.empty())
    {
      target = query_;
    }
    else
    {
      target = "/";
    }
    if (!IsValidRequestTarget(target))
    {
      return false;
    }

    Buffer serialized;
    serialized.Append(MethodString());
    serialized.Append(" ");
    serialized.Append(target);
    serialized.Append(" HTTP/1.1\r\n");

    bool has_content_length = false;
    size_t header_bytes = 0;
    for (const auto &header : headers_)
    {
      if (!IsValidHeaderName(header.first) ||
          !IsValidHeaderValue(header.second))
      {
        return false;
      }
      if (HeaderNameEquals(header.first, "Content-Length"))
      {
        has_content_length = true;
        continue;
      }
      if (HeaderNameEquals(header.first, "Transfer-Encoding"))
      {
        return false;
      }
      header_bytes += header.first.size() + header.second.size() + 4;
      if (header_bytes > 64 * 1024)
      {
        return false;
      }
      serialized.Append(header.first);
      serialized.Append(": ");
      serialized.Append(header.second);
      serialized.Append("\r\n");
    }

    if (has_content_length || !body_.empty())
    {
      serialized.Append("Content-Length: ");
      serialized.Append(std::to_string(body_.size()));
      serialized.Append("\r\n");
    }

    serialized.Append("\r\n");
    if (!body_.empty())
    {
      serialized.Append(body_.c_str(), body_.size());
    }
    output->Append(serialized.ToStringView());
    return true;
  }
} // namespace zrpc
