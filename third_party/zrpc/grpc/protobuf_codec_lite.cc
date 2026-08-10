#include <google/protobuf/message.h>
#include <zlib.h>

#include <exception>

#include "zrpc/grpc/protobuf_codec_lite.h"
#include "zrpc/grpc/buffer_stream.h"
#include "zrpc/base/logger.h"
#include "zrpc/grpc/google.h"
#include "zrpc/net/tcp_connection.h"

using namespace zrpc;
namespace {
int ProtobufVersionCheck() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  return 0;
}
[[maybe_unused]] int dummy = ProtobufVersionCheck();
}  // namespace

bool ProtobufCodecLite::Send(const std::shared_ptr<TcpConnection>& conn,
                             const ::google::protobuf::Message& message) {
  if (!conn || !conn->Connected()) {
    return false;
  }
  Buffer buf;
  FillEmptyBuffer(&buf, message);
  if (buf.ReadableBytes() <= 0) {
    return false;
  }
  conn->Send(&buf);
  return true;
}

void ProtobufCodecLite::FillEmptyBuffer(
    Buffer* buf, const google::protobuf::Message& message) {
  if (buf == nullptr || buf->ReadableBytes() != 0) {
    return;
  }
  assert(buf->ReadableBytes() == 0);
  const size_t max_payload =
      static_cast<size_t>(kMaxMessageLen - kChecksumLen) - tag_.size();
  if (message.ByteSizeLong() > max_payload) {
    return;
  }
  buf->Append(tag_);

  int byte_size = SerializeToBuffer(message, buf);
  if (byte_size < 0) {
    buf->RetrieveAll();
    return;
  }
  int32_t check_sum =
      Checksum(buf->Peek(), static_cast<int>(buf->ReadableBytes()));
  buf->AppendInt32(check_sum);
  const int32_t expected_size =
      static_cast<int32_t>(tag_.size()) + byte_size + kChecksumLen;
  assert(buf->ReadableBytes() == expected_size);
  (void)byte_size;
  int32_t len =
      socket::HostToNetwork32(static_cast<int32_t>(buf->ReadableBytes()));
  buf->Prepend(&len, sizeof len);
}

void ProtobufCodecLite::OnMessage(const std::shared_ptr<TcpConnection>& conn,
                                  Buffer* buf) {
  if (buf == nullptr) {
    DefaultErrorCallback(conn, nullptr, kParseError);
    return;
  }
  auto report_error = [&](ErrorCode error) {
    try {
      error_callback_(conn, buf, error);
    } catch (const std::exception& ex) {
      LOG_WARN << "protobuf codec error callback threw: " << ex.what();
      DefaultErrorCallback(conn, buf, error);
    } catch (...) {
      LOG_WARN << "protobuf codec error callback threw an unknown exception";
      DefaultErrorCallback(conn, buf, error);
    }
  };
  while (buf->ReadableBytes() >= kMinMessageLen + kHeaderLen) {
    const int32_t len = buf->PeekInt32();
    if (len > kMaxMessageLen || len < kMinMessageLen) {
      report_error(kInvalidLength);
      break;
    } else if (buf->ReadableBytes() >= kHeaderLen + len) {
      if (raw_callback_) {
        try {
          if (!raw_callback_(
                  conn,
                  std::string_view(buf->Peek(), kHeaderLen + len))) {
            buf->Retrieve(kHeaderLen + len);
            continue;
          }
        } catch (const std::exception& ex) {
          LOG_WARN << "protobuf raw callback threw: " << ex.what();
          DefaultErrorCallback(conn, buf, kParseError);
          break;
        } catch (...) {
          LOG_WARN << "protobuf raw callback threw an unknown exception";
          DefaultErrorCallback(conn, buf, kParseError);
          break;
        }
      }

      MessagePtr message(prototype_->New());
      ErrorCode error_code =
          Parse(buf->Peek() + kHeaderLen, len, message.get());
      if (error_code == kNoError) {
        try {
          message_callback_(conn, message);
        } catch (const std::exception& ex) {
          LOG_WARN << "protobuf message callback threw: " << ex.what();
          DefaultErrorCallback(conn, buf, kParseError);
          break;
        } catch (...) {
          LOG_WARN << "protobuf message callback threw an unknown exception";
          DefaultErrorCallback(conn, buf, kParseError);
          break;
        }
        buf->Retrieve(kHeaderLen + len);
      } else {
        report_error(error_code);
        break;
      }
    } else {
      break;
    }
  }
}

bool ProtobufCodecLite::ParseFromBuffer(std::string_view buf,
                                        google::protobuf::Message* message) {
  return message->ParseFromArray(buf.data(), static_cast<int>(buf.size()));
}

int ProtobufCodecLite::SerializeToBuffer(
    const google::protobuf::Message& message, Buffer* buf) {
  BufferOutputStream os(buf);
  if (!message.SerializeToZeroCopyStream(&os)) {
    return -1;
  }
  return static_cast<int>(os.ByteCount());
}

namespace {
const std::string kNoErrorStr = "NoError";
const std::string kInvalidLengthStr = "InvalidLength";
const std::string kCheckSumErrorStr = "CheckSumError";
const std::string kInvalidNameLenStr = "InvalidNameLen";
const std::string kUnknownMessageTypeStr = "UnknownMessageType";
const std::string kParseErrorStr = "ParseError";
const std::string kUnknownErrorStr = "UnknownError";
}  // namespace

const std::string& ProtobufCodecLite::ErrorCodeToString(ErrorCode error_code) {
  switch (error_code) {
    case kNoError:
      return kNoErrorStr;
    case kInvalidLength:
      return kInvalidLengthStr;
    case kCheckSumError:
      return kCheckSumErrorStr;
    case kInvalidNameLen:
      return kInvalidNameLenStr;
    case kUnknownMessageType:
      return kUnknownMessageTypeStr;
    case kParseError:
      return kParseErrorStr;
    default:
      return kUnknownErrorStr;
  }
}

void ProtobufCodecLite::DefaultErrorCallback(
    const std::shared_ptr<TcpConnection>& conn, Buffer* /*buf*/,
    ErrorCode error_code) {
  LOG_WARN << "protobuf codec error: " << ErrorCodeToString(error_code)
           << ", shutdown connection";
  if (conn && conn->Connected()) {
    conn->Shutdown();
  }
}

int32_t ProtobufCodecLite::AsInt32(const char* buf) {
  int32_t be32 = 0;
  ::memcpy(&be32, buf, sizeof(be32));
  return socket::NetworkToHost32(be32);
}

int32_t ProtobufCodecLite::Checksum(const void* buf, int len) {
  return static_cast<int32_t>(
      ::adler32(1, static_cast<const Bytef*>(buf), len));
}

bool ProtobufCodecLite::ValidateChecksum(const char* buf, int len) {
  if (buf == nullptr || len < kChecksumLen) {
    return false;
  }
  // check sum
  int32_t expected_checksum = AsInt32(buf + len - kChecksumLen);
  int32_t check_sum = Checksum(buf, len - kChecksumLen);
  return check_sum == expected_checksum;
}

ProtobufCodecLite::ErrorCode ProtobufCodecLite::Parse(
    const char* buf, int len, ::google::protobuf::Message* message) {
  if (buf == nullptr || message == nullptr || len < kMinMessageLen ||
      len > kMaxMessageLen) {
    return kInvalidLength;
  }
  ErrorCode error = kNoError;

  if (ValidateChecksum(buf, len)) {
    if (memcmp(buf, tag_.data(), tag_.size()) == 0) {
      // parse from buffer
      const char* data = buf + tag_.size();
      int32_t data_len = len - kChecksumLen - static_cast<int>(tag_.size());
      if (ParseFromBuffer(std::string_view(data, data_len), message)) {
        error = kNoError;
      } else {
        error = kParseError;
      }
    } else {
      error = kUnknownMessageType;
    }
  } else {
    error = kCheckSumError;
  }

  return error;
}
