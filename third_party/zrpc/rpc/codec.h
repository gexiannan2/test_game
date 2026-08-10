#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "zrpc/base/buffer.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/rpc/message.h"

namespace zrpc {
namespace rpc {

class Codec {
 public:
  static constexpr int kHeaderLen = sizeof(int32_t);
  static constexpr int kChecksumLen = sizeof(int32_t);
  static constexpr int kMaxFrameLen = 8 * 1024 * 1024;
  static constexpr int kMaxPayloadLen = kMaxFrameLen - 16 * 1024;
  static constexpr std::string_view kTag = "ZRPC";

  enum class ParseError {
    kNoError = 0,
    kInvalidLength,
    kChecksum,
    kWrongTag,
    kParseError,
  };

  using MessageCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&, const Message&)>;
  using ErrorCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*,
                         ParseError)>;

  explicit Codec(MessageCallback message_cb,
                 ErrorCallback error_cb = DefaultErrorCallback);

  bool Send(const std::shared_ptr<TcpConnection>& conn, const Message& message);
  void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf);

  static const char* ParseErrorName(ParseError error);
  static void DefaultErrorCallback(const std::shared_ptr<TcpConnection>& conn,
                                   Buffer* buf, ParseError error);

 private:
  static int32_t Checksum(const void* buf, int len);
  static bool ValidateChecksum(const char* buf, int len);
  static bool EncodePayload(const Message& message, Buffer* buf);
  static bool DecodePayload(std::string_view payload, Message* message);

  MessageCallback message_callback_;
  ErrorCallback error_callback_;
};

}  // namespace rpc
}  // namespace zrpc
