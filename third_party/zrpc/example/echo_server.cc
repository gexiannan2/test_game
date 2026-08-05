#include <cstdint>

#include "echo.pb.h"
#include "zrpc/base/buffer.h"
#include "zrpc/base/logger.h"
#include "zrpc/net/event_loop.h"
#include "zrpc/net/tcp_connection.h"
#include "zrpc/net/tcp_server.h"

using namespace zrpc;

class EchoServer {
 public:
  EchoServer(std::string ip, int port) : server_(&loop_, ip, port, nullptr) {}

  void Start() {
    server_.SetThreadNum(0);
    server_.SetConnectionCallback(std::bind(&EchoServer::ConnectionCallback,
                                            this, std::placeholders::_1));
    server_.SetMessageCallback(std::bind(&EchoServer::MessageCallback, this,
                                         std::placeholders::_1,
                                         std::placeholders::_2));
    server_.Start();
  }

  void ConnectionCallback(const std::shared_ptr<TcpConnection>& conn) {
    if (conn->Connected()) {
      if (!socket::SetKeepAlive(conn->GetSockfd(), 1)) {
        LOG_WARN << "set keepalive failed";
      }
      LOG_INFO << "connect";
    } else {
      LOG_INFO << "disconnect";
    }
  }

  void Loop() { loop_.Run(); }

  void MessageCallback(
      const std::shared_ptr<TcpConnection>& conn, Buffer* buf) {
    while (buf->ReadableBytes() >=
           static_cast<int32_t>(sizeof(uint32_t))) {
      const uint16_t data_bytes = static_cast<uint16_t>(buf->PeekInt16());
      if (data_bytes < kMsgHeadSize) {
        LOG_WARN << "invalid echo frame size: " << data_bytes;
        conn->ForceClose();
        return;
      }
      if (buf->ReadableBytes() >= data_bytes) {
        buf->RetrieveInt32();
        echo::PingRequest request;
        if (!request.ParseFromArray(buf->Peek(),
                                    data_bytes - kMsgHeadSize)) {
          LOG_WARN << "invalid echo request";
          conn->ForceClose();
          return;
        }
        buf->Retrieve(data_bytes - kMsgHeadSize);
        Buffer reply_buf;
        echo::PongRepsonse response;
        response.set_id(request.id());
        std::string serialized;
        if (!response.SerializeToString(&serialized)) {
          LOG_WARN << "serialize echo response failed";
          conn->ForceClose();
          return;
        }
        if (serialized.size() >
            static_cast<size_t>(UINT16_MAX - kMsgHeadSize)) {
          LOG_WARN << "echo response is too large";
          conn->ForceClose();
          return;
        }
        reply_buf.Append(serialized);
        reply_buf.PrependInt16(0);
        reply_buf.PrependInt16(static_cast<int16_t>(
            reply_buf.ReadableBytes() + sizeof(int16_t)));
        conn->Send(&reply_buf);
      } else {
        break;
      }
    }
  }

  static constexpr uint16_t kMsgHeadSize = sizeof(uint16_t) * 2;

 private:
  EventLoop loop_;
  TcpServer server_;
};

int main() {
  EchoServer server("0.0.0.0", 6379);
  server.Start();
  server.Loop();
}
