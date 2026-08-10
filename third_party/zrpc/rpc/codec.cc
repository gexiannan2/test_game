#include "zrpc/rpc/codec.h"

#include <zlib.h>

#include <cstring>
#include <exception>
#include <stdexcept>

#include "zrpc/base/logger.h"
#include "zrpc/net/socket.h"

namespace zrpc {
namespace rpc {
namespace {

constexpr int kMinBodyLen =
    static_cast<int>(Codec::kTag.size() + Codec::kChecksumLen);
constexpr uint16_t kMaxNameLen = 4096;

bool IsValidMessageType(MessageType type) {
  return type == MessageType::kRequest || type == MessageType::kResponse;
}

bool IsValidErrorCode(ErrorCode error) {
  switch (error) {
    case ErrorCode::kOk:
    case ErrorCode::kWrongProto:
    case ErrorCode::kNoService:
    case ErrorCode::kNoMethod:
    case ErrorCode::kInvalidRequest:
    case ErrorCode::kInvalidResponse:
    case ErrorCode::kTimeout:
    case ErrorCode::kTransport:
    case ErrorCode::kRejected:
      return true;
    default:
      return false;
  }
}

bool AppendString(Buffer* buf, const std::string& value) {
  if (value.size() > kMaxNameLen) {
    return false;
  }
  buf->AppendInt16(static_cast<int16_t>(value.size()));
  if (!value.empty()) {
    buf->Append(value.data(), static_cast<int32_t>(value.size()));
  }
  return true;
}

bool ReadString(Buffer* buf, std::string* out) {
  if (buf->ReadableBytes() < static_cast<int32_t>(sizeof(int16_t))) {
    return false;
  }
  const int16_t len = buf->PeekInt16();
  if (len < 0 || len > kMaxNameLen) {
    return false;
  }
  if (buf->ReadableBytes() < static_cast<int32_t>(sizeof(int16_t) + len)) {
    return false;
  }
  buf->RetrieveInt16();
  if (len == 0) {
    out->clear();
    return true;
  }
  *out = buf->RetrieveAsString(len);
  return true;
}

}  // namespace

Codec::Codec(MessageCallback message_cb, ErrorCallback error_cb)
    : message_callback_(std::move(message_cb)),
      error_callback_(std::move(error_cb)) {
  if (!message_callback_) {
    throw std::invalid_argument("rpc codec requires a message callback");
  }
  if (!error_callback_) {
    error_callback_ = DefaultErrorCallback;
  }
}

bool Codec::Send(const std::shared_ptr<TcpConnection>& conn,
                 const Message& message) {
  if (!conn || !conn->Connected()) {
    return false;
  }
  Buffer buf;
  buf.Append(kTag);
  if (!EncodePayload(message, &buf)) {
    LOG_WARN << "rpc codec encode failed";
    return false;
  }
  if (buf.ReadableBytes() + kChecksumLen > kMaxFrameLen) {
    LOG_WARN << "rpc codec frame exceeds maximum length";
    return false;
  }
  const int32_t checksum =
      Checksum(buf.Peek(), static_cast<int>(buf.ReadableBytes()));
  buf.AppendInt32(checksum);
  const int32_t len =
      socket::HostToNetwork32(static_cast<int32_t>(buf.ReadableBytes()));
  buf.Prepend(&len, sizeof len);
  conn->Send(&buf);
  return true;
}

bool Codec::EncodePayload(const Message& message, Buffer* buf) {
  if (!IsValidMessageType(message.type) ||
      !IsValidErrorCode(message.error) ||
      (message.type == MessageType::kRequest &&
       message.error != ErrorCode::kOk)) {
    return false;
  }
  buf->AppendInt8(static_cast<int8_t>(message.type));
  buf->AppendInt64(message.id);
  buf->AppendInt16(static_cast<int16_t>(message.error));
  if (!AppendString(buf, message.service) ||
      !AppendString(buf, message.method)) {
    return false;
  }
  if (message.body.size() > kMaxPayloadLen) {
    return false;
  }
  buf->AppendInt32(static_cast<int32_t>(message.body.size()));
  if (!message.body.empty()) {
    buf->Append(message.body.data(),
                static_cast<int32_t>(message.body.size()));
  }
  return true;
}

bool Codec::DecodePayload(std::string_view payload, Message* message) {
  if (message == nullptr ||
      payload.size() >
          static_cast<size_t>(kMaxFrameLen - kChecksumLen - kTag.size())) {
    return false;
  }

  Buffer buf;
  buf.Append(payload.data(), static_cast<int32_t>(payload.size()));

  if (buf.ReadableBytes() < static_cast<int32_t>(sizeof(int8_t) + sizeof(int64_t) +
                                                 sizeof(int16_t))) {
    return false;
  }
  message->type = static_cast<MessageType>(buf.ReadInt8());
  message->id = buf.ReadInt64();
  message->error = static_cast<ErrorCode>(buf.ReadInt16());
  if (!IsValidMessageType(message->type) ||
      !IsValidErrorCode(message->error) ||
      (message->type == MessageType::kRequest &&
       message->error != ErrorCode::kOk)) {
    return false;
  }
  if (!ReadString(&buf, &message->service) ||
      !ReadString(&buf, &message->method)) {
    return false;
  }
  if (buf.ReadableBytes() < static_cast<int32_t>(sizeof(int32_t))) {
    return false;
  }
  const int32_t body_len = buf.ReadInt32();
  if (body_len < 0 || body_len > kMaxPayloadLen ||
      buf.ReadableBytes() < body_len) {
    return false;
  }
  message->body = buf.RetrieveAsString(body_len);
  return buf.ReadableBytes() == 0;
}

void Codec::OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
  if (buf == nullptr) {
    DefaultErrorCallback(conn, nullptr, ParseError::kParseError);
    return;
  }
  auto report_error = [&](ParseError error) {
    try {
      error_callback_(conn, buf, error);
    } catch (const std::exception& ex) {
      LOG_WARN << "rpc codec error callback threw: " << ex.what();
      DefaultErrorCallback(conn, buf, error);
    } catch (...) {
      LOG_WARN << "rpc codec error callback threw an unknown exception";
      DefaultErrorCallback(conn, buf, error);
    }
  };
  while (buf->ReadableBytes() >= kMinBodyLen + kHeaderLen) {
    const int32_t len = buf->PeekInt32();
    if (len > kMaxFrameLen || len < kMinBodyLen) {
      report_error(ParseError::kInvalidLength);
      break;
    }
    if (buf->ReadableBytes() < kHeaderLen + len) {
      break;
    }

    const char* frame = buf->Peek() + kHeaderLen;
    if (!ValidateChecksum(frame, len)) {
      report_error(ParseError::kChecksum);
      break;
    }
    if (memcmp(frame, kTag.data(), kTag.size()) != 0) {
      report_error(ParseError::kWrongTag);
      break;
    }

    Message message;
    const std::string_view payload(frame + kTag.size(),
                                   len - kChecksumLen - kTag.size());
    if (!DecodePayload(payload, &message)) {
      report_error(ParseError::kParseError);
      break;
    }

    try {
      message_callback_(conn, message);
    } catch (const std::exception& ex) {
      LOG_WARN << "rpc codec message callback threw: " << ex.what();
      DefaultErrorCallback(conn, buf, ParseError::kParseError);
      break;
    } catch (...) {
      LOG_WARN << "rpc codec message callback threw an unknown exception";
      DefaultErrorCallback(conn, buf, ParseError::kParseError);
      break;
    }
    buf->Retrieve(kHeaderLen + len);
  }
}

const char* Codec::ParseErrorName(ParseError error) {
  switch (error) {
    case ParseError::kNoError:
      return "NoError";
    case ParseError::kInvalidLength:
      return "InvalidLength";
    case ParseError::kChecksum:
      return "Checksum";
    case ParseError::kWrongTag:
      return "WrongTag";
    case ParseError::kParseError:
      return "ParseError";
    default:
      return "Unknown";
  }
}

void Codec::DefaultErrorCallback(const std::shared_ptr<TcpConnection>& conn,
                                 Buffer* /*buf*/, ParseError error) {
  LOG_WARN << "native rpc codec error: " << ParseErrorName(error)
           << ", shutdown connection";
  if (conn && conn->Connected()) {
    conn->Shutdown();
  }
}

int32_t Codec::Checksum(const void* buf, int len) {
  return static_cast<int32_t>(
      ::adler32(1, static_cast<const Bytef*>(buf), len));
}

bool Codec::ValidateChecksum(const char* buf, int len) {
  int32_t expected = 0;
  ::memcpy(&expected, buf + len - kChecksumLen, sizeof expected);
  expected = socket::NetworkToHost32(expected);
  return Checksum(buf, len - kChecksumLen) == expected;
}

}  // namespace rpc
}  // namespace zrpc
