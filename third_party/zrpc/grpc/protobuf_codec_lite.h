#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "zrpc/net/callback.h"

namespace google {
namespace protobuf {
class Message;
}
}  // namespace google
namespace zrpc {

typedef std::shared_ptr<google::protobuf::Message> MessagePtr;

class ProtobufCodecLite {
 public:
  const static int kHeaderLen = sizeof(int32_t);
  const static int kChecksumLen = sizeof(int32_t);
  const static int kMaxMessageLen = 8 * 1024 * 1024;

  enum ErrorCode {
    kNoError = 0,
    kInvalidLength,
    kCheckSumError,
    kInvalidNameLen,
    kUnknownMessageType,
    kParseError,
  };

  typedef std::function<bool(const std::shared_ptr<TcpConnection>&,
                             std::string_view)>
      RawMessageCallback;

  typedef std::function<void(const std::shared_ptr<TcpConnection>&,
                             const MessagePtr&)>
      ProtobufMessageCallback;

  typedef std::function<void(const std::shared_ptr<TcpConnection>&, Buffer*,
                             ErrorCode)>
      ErrorCallback;

  ProtobufCodecLite(const ::google::protobuf::Message* prototype,
                    std::string_view tag_arg,
                    const ProtobufMessageCallback& message_cb,
                    const RawMessageCallback& raw_cb = RawMessageCallback(),
                    const ErrorCallback& error_cb = DefaultErrorCallback)
      : prototype_(prototype),
        tag_(tag_arg),
        message_callback_(message_cb),
        raw_callback_(raw_cb),
        error_callback_(error_cb),
        kMinMessageLen(tag_arg.size() + kChecksumLen) {
    if (prototype_ == nullptr || tag_.empty() || !message_callback_ ||
        tag_.size() >
            static_cast<size_t>(kMaxMessageLen - kChecksumLen)) {
      throw std::invalid_argument("invalid protobuf codec configuration");
    }
    if (!error_callback_) {
      error_callback_ = DefaultErrorCallback;
    }
  }

  ProtobufCodecLite(const ProtobufCodecLite&) = delete;
  ProtobufCodecLite& operator=(const ProtobufCodecLite&) = delete;
  ProtobufCodecLite(ProtobufCodecLite&&) = delete;
  ProtobufCodecLite& operator=(ProtobufCodecLite&&) = delete;

  virtual ~ProtobufCodecLite() = default;

  const std::string& tag() const { return tag_; }
  bool Send(const std::shared_ptr<TcpConnection>& conn,
            const ::google::protobuf::Message& message);
  void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf);
  virtual bool ParseFromBuffer(std::string_view buf,
                               google::protobuf::Message* message);
  virtual int SerializeToBuffer(const google::protobuf::Message& message,
                                Buffer* buf);
  static const std::string& ErrorCodeToString(ErrorCode error_ode);

  // public for unit tests
  ErrorCode Parse(const char* buf, int len,
                  ::google::protobuf::Message* message);
  void FillEmptyBuffer(Buffer* buf, const google::protobuf::Message& message);

  static int32_t Checksum(const void* buf, int len);
  static bool ValidateChecksum(const char* buf, int len);
  static int32_t AsInt32(const char* buf);
  static void DefaultErrorCallback(const std::shared_ptr<TcpConnection>&,
                                   Buffer*, ErrorCode);

 private:
  const ::google::protobuf::Message* prototype_;
  const std::string tag_;
  ProtobufMessageCallback message_callback_;
  RawMessageCallback raw_callback_;
  ErrorCallback error_callback_;
  const int kMinMessageLen;
};

template <typename MSG, const char* TAG,
          typename CODEC =
              ProtobufCodecLite>  // TAG must be a variable with external
                                  // linkage, not a string literal
class ProtobufCodecLiteT {
  static_assert(std::is_base_of<ProtobufCodecLite, CODEC>::value,
                "CODEC should be derived from ProtobufCodecLite");

 public:
  typedef std::shared_ptr<MSG> ConcreteMessagePtr;
  typedef std::function<void(const std::shared_ptr<TcpConnection>&,
                             const ConcreteMessagePtr&)>
      ProtobufMessageCallback;
  typedef ProtobufCodecLite::RawMessageCallback RawMessageCallback;
  typedef ProtobufCodecLite::ErrorCallback ErrorCallback;

  explicit ProtobufCodecLiteT(
      const ProtobufMessageCallback& message_cb,
      const RawMessageCallback& raw_cb = RawMessageCallback(),
      const ErrorCallback& error_cb = ProtobufCodecLite::DefaultErrorCallback)
      : message_callback_(message_cb),
        codec_(&MSG::default_instance(), TAG,
               std::bind(&ProtobufCodecLiteT::OnRpcMessage, this,
                         std::placeholders::_1, std::placeholders::_2),
                raw_cb, error_cb) {
    if (!message_callback_) {
      throw std::invalid_argument(
          "protobuf codec requires a message callback");
    }
  }

  ProtobufCodecLiteT(const ProtobufCodecLiteT&) = delete;
  ProtobufCodecLiteT& operator=(const ProtobufCodecLiteT&) = delete;
  ProtobufCodecLiteT(ProtobufCodecLiteT&&) = delete;
  ProtobufCodecLiteT& operator=(ProtobufCodecLiteT&&) = delete;

  const std::string& tag() const { return codec_.tag(); }

  bool Send(const std::shared_ptr<TcpConnection>& conn, const MSG& message) {
    return codec_.Send(conn, message);
  }

  void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
    codec_.OnMessage(conn, buf);
  }

  void OnRpcMessage(const std::shared_ptr<TcpConnection>& conn,
                    const MessagePtr& message) {
    message_callback_(conn, std::static_pointer_cast<MSG>(message));
  }

  void FillEmptyBuffer(Buffer* buf, const MSG& message) {
    codec_.FillEmptyBuffer(buf, message);
  }

 private:
  ProtobufMessageCallback message_callback_;
  CODEC codec_;
};

}  // namespace zrpc
