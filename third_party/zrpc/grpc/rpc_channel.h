#pragma once



#include <google/protobuf/service.h>



#include <atomic>

#include <condition_variable>

#include <map>

#include <memory>

#include <mutex>

#include <string>



#include "zrpc/grpc/protobuf_codec_lite.h"
#include "zrpc/grpc/rpc_metrics.h"

#include "zrpc/grpc/rpc.pb.h"

#include "zrpc/net/tcp_connection.h"



extern const char rpctag[];  // = "RPC0";

namespace zrpc {

class Timer;

// wire format

//

// Field     Length  Content

//

// size      4-byte  N+8

// "RPC0"    4-byte

// payload   N-byte

// checksum  4-byte  adler32 of "RPC0"+payload

//



typedef ProtobufCodecLiteT<RpcMessage, rpctag> RpcCodec;

struct PendingResponse {
  const ::google::protobuf::Message* request = nullptr;
  ::google::protobuf::Message* response = nullptr;
  ::google::protobuf::RpcController* controller = nullptr;
};

struct ServerInflight {
  std::mutex mutex;
  PendingResponse* pending = nullptr;
  bool done = false;
  bool response_sent = false;
  std::shared_ptr<Timer> watchdog;
};

class EventLoop;



const char* RpcErrorCodeName(ErrorCode code);



}  // namespace zrpc



namespace google {

namespace protobuf {



class Descriptor;

class ServiceDescriptor;

class MethodDescriptor;

class Message;

class Closure;

class RpcController;

class Service;



}  // namespace protobuf

}  // namespace google



namespace zrpc {

class RpcChannel : public ::google::protobuf::RpcChannel,

                   public std::enable_shared_from_this<RpcChannel> {

 public:

  RpcChannel();



  explicit RpcChannel(const std::shared_ptr<TcpConnection>& conn);



  ~RpcChannel() override;



  void SetConnection(const std::shared_ptr<TcpConnection>& conn);

  void OnDisconnect();
  void PrepareShutdown();

  void SetMetrics(const std::shared_ptr<RpcMetrics>& metrics) {
    metrics_ = metrics;
  }

  bool Connected() const;



  void SetServices(

      const std::map<std::string, ::google::protobuf::Service*>* services) {

    if (services != nullptr) {
      services_ = *services;
      accepts_requests_.store(true, std::memory_order_release);
    }

  }



  void CallMethod(const ::google::protobuf::MethodDescriptor* method,

                  ::google::protobuf::RpcController* controller,

                  const ::google::protobuf::Message* request,

                  ::google::protobuf::Message* response,

                  ::google::protobuf::Closure* done) override;



  void OnMessage(const std::shared_ptr<TcpConnection>& conn, Buffer* buf);

  void DoneCallback(const std::shared_ptr<struct ServerInflight>& inflight,
                    int64_t id);
  void OnServerRequestTimeout(const std::shared_ptr<struct ServerInflight>& inflight,
                              int64_t id);

 private:

  struct OutstandingCall {

    ::google::protobuf::RpcController* controller = nullptr;

    ::google::protobuf::Message* response = nullptr;

    ::google::protobuf::Closure* done = nullptr;

    bool sync = false;

    std::shared_ptr<Timer> timeout_timer;

    // 同步调用不能以“已从映射删除”作为完成条件：响应线程会先删除映射，
    // 再填写 response/controller。独立完成标记用于发布最终结果。
    std::shared_ptr<std::atomic<bool>> completed;

  };



  void OnRpcMessage(const std::shared_ptr<TcpConnection>& conn,

                    const std::shared_ptr<RpcMessage>& message);



  bool RemoveOutstanding(int64_t id, OutstandingCall* out);

  void FailOutstanding(int64_t id, const std::string& reason, ErrorCode code);

  void FailAllOutstanding(const std::string& reason, ErrorCode code);

  void CompleteResponse(const OutstandingCall& out,

                        const std::shared_ptr<RpcMessage>& message);

  void WaitSyncResponse(int64_t id, double timeout_sec,
                        const std::shared_ptr<std::atomic<bool>>& completed);

  void OnCallTimeout(int64_t id);

  void ScheduleTimeout(int64_t id, double timeout_sec);

  void SetControllerFailed(::google::protobuf::RpcController* controller,

                           const std::string& reason, ErrorCode code);
  std::shared_ptr<TcpConnection> ConnectionSnapshot() const;



  RpcCodec codec_;

  mutable std::mutex connection_mutex_;
  std::shared_ptr<TcpConnection> conn_;

  std::atomic<int64_t> id_{1};

  std::atomic<bool> disconnected_{false};
  std::atomic<bool> closing_{false};
  std::atomic<int32_t> server_inflight_{0};

  std::mutex mutex_;
  std::condition_variable sync_cv_;

  std::map<int64_t, OutstandingCall> outstandings_;

  std::map<std::string, ::google::protobuf::Service*> services_;
  std::atomic<bool> accepts_requests_{false};

  std::shared_ptr<RpcMetrics> metrics_;
};

typedef std::shared_ptr<RpcChannel> RpcChannelPtr;



}  // namespace zrpc

