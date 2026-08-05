#include <chrono>
#include <cstdint>

#include "echo.pb.h"
#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_client.h"
#include "zrpc/net/tcp_connection.h"

using namespace zrpc;

class EchoClient {
 public:
  EchoClient(std::string ip, int port)
      : client_(&loop_, ip, port, nullptr), count_(0) {}

  void Start() {
    for (int i = 0; i < 500; ++i) {
      message_.push_back(static_cast<char>(i % 128));
    }

    client_.SetConnectionCallback(std::bind(&EchoClient::ConnectionCallback,
                                            this, std::placeholders::_1));
    client_.SetMessageCallback(std::bind(&EchoClient::MessageCallback, this,
                                         std::placeholders::_1,
                                         std::placeholders::_2));
    client_.Connect();
  }

  void ConnectionCallback(const std::shared_ptr<TcpConnection>& conn) {
    if (conn->Connected()) {
      if (!socket::SetKeepAlive(conn->GetSockfd(), 1)) {
        LOG_WARN << "设置 keepalive 失败";
      }
      started_at_ = std::chrono::steady_clock::now();
      if (!SendRequest(conn)) {
        conn->ForceClose();
        loop_.Quit();
        return;
      }
      LOG_INFO << "连接服务器成功";
    } else {
      LOG_INFO << "断开服务器连接";
    }
  }

  void Loop() { loop_.Run(); }

  void MessageCallback(const std::shared_ptr<TcpConnection>& conn,
                       Buffer* buf) {
    while (buf->ReadableBytes() >=
           static_cast<int32_t>(sizeof(uint32_t))) {
      const uint16_t data_bytes = static_cast<uint16_t>(buf->PeekInt16());
      if (data_bytes < kMsgHeadSize) {
        LOG_WARN << "无效响应帧长度: " << data_bytes;
        conn->ForceClose();
        loop_.Quit();
        return;
      }
      if (buf->ReadableBytes() >= data_bytes) {
        buf->RetrieveInt32();
        echo::PongRepsonse response;
        if (!response.ParseFromArray(buf->Peek(),
                                     data_bytes - kMsgHeadSize) ||
            response.id() != message_) {
          LOG_WARN << "无效 echo 响应";
          conn->ForceClose();
          loop_.Quit();
          return;
        }
        buf->Retrieve(data_bytes - kMsgHeadSize);

        ++count_;
        if (count_ < 500) {
          if (!SendRequest(conn)) {
            conn->ForceClose();
            loop_.Quit();
            return;
          }
        } else {
          const auto elapsed = std::chrono::duration_cast<
              std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                         started_at_);
          LOG_INFO << "500 次 echo 耗时毫秒: " << elapsed.count();
          conn->Shutdown();
          loop_.Quit();
        }
      } else {
        break;
      }
    }
  }

  bool SendRequest(const std::shared_ptr<TcpConnection>& conn) {
    echo::PingRequest request;
    request.set_id(message_);
    std::string serialized;
    if (!request.SerializeToString(&serialized) ||
        serialized.size() >
            static_cast<size_t>(UINT16_MAX - kMsgHeadSize)) {
      LOG_WARN << "序列化 echo 请求失败";
      return false;
    }
    Buffer buffer;
    buffer.Append(serialized);
    buffer.PrependInt16(0);
    buffer.PrependInt16(static_cast<int16_t>(
        buffer.ReadableBytes() + sizeof(int16_t)));
    conn->Send(&buffer);
    return true;
  }

  static constexpr uint16_t kMsgHeadSize = sizeof(uint16_t) * 2;

 private:
  EventLoop loop_;
  TcpClient client_;
  std::string message_;
  uint32_t count_;
  std::chrono::steady_clock::time_point started_at_;
};

int main() {
  EchoClient client("127.0.0.1", 6379);
  client.Start();
  client.Loop();
}
