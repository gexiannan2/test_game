#include "zrpc/grpc/rpc_channel.h"



#include <google/protobuf/descriptor.h>

#include <google/protobuf/stubs/callback.h>



#include <chrono>
#include <exception>



#include "zrpc/base/logger.h"

#include "zrpc/base/timer.h"

#include "zrpc/net/event_loop.h"

#include "zrpc/grpc/rpc_controller.h"



const char rpctag[] = "RPC0";



namespace zrpc {

namespace {

constexpr double kServerRequestTimeoutSec = 120.0;

struct OwnedPendingResponse {
  std::unique_ptr<PendingResponse> pending;
  std::unique_ptr<const ::google::protobuf::Message> request;
  std::unique_ptr<::google::protobuf::Message> response;
  std::unique_ptr<::google::protobuf::RpcController> controller;
};

OwnedPendingResponse TakePending(PendingResponse* pending) {
  OwnedPendingResponse owned;
  if (pending == nullptr) {
    return owned;
  }
  owned.pending.reset(pending);
  owned.request.reset(pending->request);
  owned.response.reset(pending->response);
  owned.controller.reset(pending->controller);
  pending->request = nullptr;
  pending->response = nullptr;
  pending->controller = nullptr;
  return owned;
}

class RpcDoneClosure : public ::google::protobuf::Closure {
 public:
  RpcDoneClosure(std::weak_ptr<RpcChannel> channel,
                 const std::shared_ptr<ServerInflight>& inflight, int64_t id)
      : channel_(std::move(channel)), inflight_(inflight), id_(id) {}

  void Run() override {
    if (auto channel = channel_.lock()) {
      channel->DoneCallback(inflight_, id_);
    } else if (inflight_) {
      std::lock_guard<std::mutex> lock(inflight_->mutex);
      if (!inflight_->done) {
        inflight_->done = true;
        TakePending(inflight_->pending);
        inflight_->pending = nullptr;
      }
    }
    delete this;
  }

 private:
  std::weak_ptr<RpcChannel> channel_;
  std::shared_ptr<ServerInflight> inflight_;
  int64_t id_;
};

double NowSeconds() {

  using clock = std::chrono::steady_clock;

  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();

}

void RunClosureSafely(::google::protobuf::Closure* done) {
  if (done == nullptr) {
    return;
  }
  try {
    done->Run();
  } catch (const std::exception& ex) {
    LOG_WARN << "rpc completion callback threw: " << ex.what();
  } catch (...) {
    LOG_WARN << "rpc completion callback threw an unknown exception";
  }
}



}  // namespace



const char* RpcErrorCodeName(ErrorCode code) {

  switch (code) {

    case NO_ERROR:

      return "NO_ERROR";

    case WRONG_PROTO:

      return "WRONG_PROTO";

    case NO_SERVICE:

      return "NO_SERVICE";

    case NO_METHOD:

      return "NO_METHOD";

    case INVALID_REQUEST:

      return "INVALID_REQUEST";

    case INVALID_RESPONSE:

      return "INVALID_RESPONSE";

    case TIMEOUT:

      return "TIMEOUT";

    default:

      return "UNKNOWN";

  }

}



RpcChannel::RpcChannel()

    : codec_(std::bind(&RpcChannel::OnRpcMessage, this, std::placeholders::_1,

                       std::placeholders::_2)) {}



RpcChannel::RpcChannel(const std::shared_ptr<TcpConnection>& conn)

    : codec_(std::bind(&RpcChannel::OnRpcMessage, this, std::placeholders::_1,

                       std::placeholders::_2)),

      conn_(conn) {}



RpcChannel::~RpcChannel() {
  closing_.store(true, std::memory_order_release);
  accepts_requests_.store(false, std::memory_order_release);
  FailAllOutstanding("rpc channel destroyed", TIMEOUT);
}

void RpcChannel::PrepareShutdown() {
  closing_.store(true, std::memory_order_release);
  accepts_requests_.store(false, std::memory_order_release);
  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (conn && conn->GetLoop()) {
    std::weak_ptr<RpcChannel> weak = weak_from_this();
    conn->GetLoop()->RunInLoop([weak, conn]() {
      if (auto self = weak.lock()) {
        if (conn->Connected()) {
          conn->Shutdown();
        }
        self->disconnected_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lk(self->connection_mutex_);
        self->conn_.reset();
      }
    });
  } else {
    OnDisconnect();
  }
}



void RpcChannel::SetConnection(const std::shared_ptr<TcpConnection>& conn) {

  {
    std::lock_guard<std::mutex> lk(connection_mutex_);
    conn_ = conn;
  }

  disconnected_.store(false, std::memory_order_release);

}



void RpcChannel::OnDisconnect() {

  disconnected_.store(true, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lk(connection_mutex_);
    conn_.reset();
  }

  FailAllOutstanding("connection closed", TIMEOUT);

}

bool RpcChannel::Connected() const {
  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  return !disconnected_.load(std::memory_order_acquire) && conn &&
         conn->Connected();
}

std::shared_ptr<TcpConnection> RpcChannel::ConnectionSnapshot() const {
  std::lock_guard<std::mutex> lk(connection_mutex_);
  return conn_;
}



void RpcChannel::SetControllerFailed(

    ::google::protobuf::RpcController* controller, const std::string& reason,

    ErrorCode code) {

  if (controller == nullptr) {

    return;

  }

  controller->SetFailed(reason);

  if (auto* ctrl = dynamic_cast<RpcController*>(controller)) {

    ctrl->SetErrorCode(static_cast<int>(code));

  }

}



bool RpcChannel::RemoveOutstanding(int64_t id, OutstandingCall* out) {

  std::unique_lock<std::mutex> lk(mutex_);

  auto it = outstandings_.find(id);

  if (it == outstandings_.end()) {

    return false;

  }

  if (out != nullptr) {

    *out = it->second;

  }

  outstandings_.erase(it);

  return true;

}



void RpcChannel::FailOutstanding(int64_t id, const std::string& reason,

                                 ErrorCode code) {

  OutstandingCall out;

  if (!RemoveOutstanding(id, &out)) {

    return;

  }



  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (out.timeout_timer && conn && conn->GetLoop()) {

    conn->GetLoop()->CancelAfter(out.timeout_timer);

  }



  SetControllerFailed(out.controller, reason, code);

  RunClosureSafely(out.done);
  if (out.completed) {
    out.completed->store(true, std::memory_order_release);
  }
  if (out.sync) {
    sync_cv_.notify_all();
  }

}



void RpcChannel::FailAllOutstanding(const std::string& reason, ErrorCode code) {

  std::map<int64_t, OutstandingCall> pending;

  {

    std::unique_lock<std::mutex> lk(mutex_);

    pending.swap(outstandings_);

  }



  for (auto& item : pending) {

    OutstandingCall& out = item.second;

    std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
    if (out.timeout_timer && conn && conn->GetLoop()) {

      conn->GetLoop()->CancelAfter(out.timeout_timer);

    }

    SetControllerFailed(out.controller, reason, code);

    RunClosureSafely(out.done);
    if (out.completed) {
      out.completed->store(true, std::memory_order_release);
    }

  }

  sync_cv_.notify_all();

}



void RpcChannel::ScheduleTimeout(int64_t id, double timeout_sec) {

  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (timeout_sec <= 0.0 || !conn || !conn->GetLoop()) {

    return;

  }



  EventLoop* loop = conn->GetLoop();

  std::weak_ptr<RpcChannel> weak = shared_from_this();

  std::shared_ptr<Timer> timer = loop->RunAfter(

      timeout_sec, false, [weak, id]() {

        if (auto self = weak.lock()) {

          self->OnCallTimeout(id);

        }

      });



  std::unique_lock<std::mutex> lk(mutex_);

  auto it = outstandings_.find(id);

  if (it != outstandings_.end()) {

    it->second.timeout_timer = std::move(timer);
    return;
  }
  lk.unlock();
  loop->CancelAfter(timer);

}



void RpcChannel::OnCallTimeout(int64_t id) {

  FailOutstanding(id, "rpc call timeout", TIMEOUT);

}



void RpcChannel::CompleteResponse(

    const OutstandingCall& out,

    const std::shared_ptr<RpcMessage>& message) {

  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (out.timeout_timer && conn && conn->GetLoop()) {

    conn->GetLoop()->CancelAfter(out.timeout_timer);

  }



  if (message->error() != NO_ERROR) {

    SetControllerFailed(out.controller,

                        std::string("rpc error: ") +

                            RpcErrorCodeName(message->error()),

                        message->error());

  } else if (!out.response->ParseFromString(message->response())) {

    SetControllerFailed(out.controller, "failed to parse rpc response",

                      INVALID_RESPONSE);

  }



  RunClosureSafely(out.done);
  if (out.completed) {
    out.completed->store(true, std::memory_order_release);
  }
  if (out.sync) {
    sync_cv_.notify_all();
  }

}



void RpcChannel::WaitSyncResponse(
    int64_t id, double timeout_sec,
    const std::shared_ptr<std::atomic<bool>>& completed) {
  if (!completed) {
    return;
  }

  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  EventLoop* loop = conn ? conn->GetLoop() : nullptr;
  const double deadline =
      timeout_sec > 0.0 ? NowSeconds() + timeout_sec : 0.0;

  if (loop != nullptr && loop->IsInLoopThread()) {
    bool timeout_requested = false;
    while (!completed->load(std::memory_order_acquire)) {
      int wait_ms = 100;
      if (deadline > 0.0 && !timeout_requested) {
        const double remain = deadline - NowSeconds();
        if (remain <= 0.0) {
          FailOutstanding(id, "rpc call timeout", TIMEOUT);
          timeout_requested = true;
          continue;
        }
        wait_ms = static_cast<int>(remain * 1000.0);
        if (wait_ms < 1) {
          wait_ms = 1;
        }
      }
      loop->PollOnce(wait_ms);
    }
    return;
  }

  std::unique_lock<std::mutex> lk(mutex_);
  if (deadline > 0.0) {
    const auto wait_deadline = std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(deadline)));
    if (!sync_cv_.wait_until(
            lk, wait_deadline,
            [&completed]() {
              return completed->load(std::memory_order_acquire);
            })) {
      lk.unlock();
      FailOutstanding(id, "rpc call timeout", TIMEOUT);
      lk.lock();
      sync_cv_.wait(lk, [&completed]() {
        return completed->load(std::memory_order_acquire);
      });
    }
  } else {
    sync_cv_.wait(lk, [&completed]() {
      return completed->load(std::memory_order_acquire);
    });
  }
}



void RpcChannel::CallMethod(

    const ::google::protobuf::MethodDescriptor* method,

    google::protobuf::RpcController* controller,

    const ::google::protobuf::Message* request,

    ::google::protobuf::Message* response,

    ::google::protobuf::Closure* done) {

  if (method == nullptr || request == nullptr || response == nullptr) {
    SetControllerFailed(controller, "invalid rpc call arguments", WRONG_PROTO);
    RunClosureSafely(done);
    return;
  }

  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (disconnected_.load(std::memory_order_acquire) || !conn ||

      !conn->Connected()) {

    SetControllerFailed(controller, "connection not ready", TIMEOUT);

    RunClosureSafely(done);

    return;

  }



  double timeout_sec = 0.0;

  if (auto* ctrl = dynamic_cast<RpcController*>(controller)) {

    timeout_sec = ctrl->TimeoutSeconds();

  }



  const int64_t id = id_.fetch_add(1, std::memory_order_relaxed);

  const bool sync = (done == nullptr);
  auto completed =
      sync ? std::make_shared<std::atomic<bool>>(false) : nullptr;



  OutstandingCall out{controller, response, done, sync, nullptr, completed};

  {

    std::unique_lock<std::mutex> lk(mutex_);

    outstandings_[id] = out;

  }



  RpcMessage message;

  message.set_type(REQUEST);

  message.set_id(id);

  message.set_service(method->service()->full_name());

  message.set_method(method->name());

  std::string request_payload;
  if (!request->SerializeToString(&request_payload)) {
    FailOutstanding(id, "failed to serialize rpc request", INVALID_REQUEST);
    return;
  }
  message.set_request(std::move(request_payload));



  if (!codec_.Send(conn, message)) {
    FailOutstanding(id, "failed to send rpc request", INVALID_REQUEST);
    return;
  }

  ScheduleTimeout(id, timeout_sec);



  if (sync) {

    WaitSyncResponse(id, timeout_sec, completed);

  }

}



void RpcChannel::OnMessage(const std::shared_ptr<TcpConnection>& conn,

                           Buffer* buf) {

  codec_.OnMessage(conn, buf);

}



void RpcChannel::OnRpcMessage(const std::shared_ptr<TcpConnection>& conn,

                              const std::shared_ptr<RpcMessage>& message) {

  if (closing_.load(std::memory_order_acquire)) {
    return;
  }

  if (conn != ConnectionSnapshot()) {

    LOG_WARN << "rpc message from unexpected connection";

    return;

  }



  if (message->type() == RESPONSE) {

    const int64_t id = message->id();

    OutstandingCall out;

    if (!RemoveOutstanding(id, &out)) {

      LOG_WARN << "unexpected rpc response id=" << id;

      return;

    }

    CompleteResponse(out, message);

    return;

  }



  if (message->type() != REQUEST) {

    return;

  }

  if (closing_.load(std::memory_order_acquire) ||
      !accepts_requests_.load(std::memory_order_acquire)) {
    RpcMessage response;
    response.set_type(RESPONSE);
    response.set_id(message->id());
    response.set_error(NO_SERVICE);
    if (conn && conn->Connected()) {
      codec_.Send(conn, response);
    }
    return;
  }

  if (metrics_) {
    metrics_->RecordServerRequest();
  }

  ErrorCode error = WRONG_PROTO;

  if (accepts_requests_.load(std::memory_order_acquire)) {

    const auto it = services_.find(message->service());

    if (it != services_.end()) {

      google::protobuf::Service* service = it->second;

      const google::protobuf::ServiceDescriptor* desc =

          service->GetDescriptor();

      const google::protobuf::MethodDescriptor* method =

          desc->FindMethodByName(message->method());

      if (method != nullptr) {

        std::unique_ptr<google::protobuf::Message> request(

            service->GetRequestPrototype(method).New());

        if (request->ParseFromString(message->request())) {

          google::protobuf::Message* response =
              service->GetResponsePrototype(method).New();
          auto* controller = new RpcController();
          auto inflight = std::make_shared<ServerInflight>();
          const google::protobuf::Message* request_ptr = request.release();
          inflight->pending =
              new PendingResponse{request_ptr, response, controller};
          const int64_t id = message->id();
          server_inflight_.fetch_add(1, std::memory_order_relaxed);
          EventLoop* loop = conn->GetLoop();
          std::weak_ptr<RpcChannel> weak = weak_from_this();
          std::weak_ptr<ServerInflight> weak_inflight = inflight;
          inflight->watchdog = loop->RunAfter(
              kServerRequestTimeoutSec, false,
              [weak, weak_inflight, id]() {
                auto self = weak.lock();
                auto current = weak_inflight.lock();
                if (self && current) {
                  self->OnServerRequestTimeout(current, id);
                }
              });
          auto* done =
              new RpcDoneClosure(weak_from_this(), inflight, id);
          try {
            service->CallMethod(method, controller, request_ptr, response,
                                done);
          } catch (const std::exception& ex) {
            controller->SetFailed(
                std::string("rpc service threw: ") + ex.what());
            done->Run();
          } catch (...) {
            controller->SetFailed(
                "rpc service threw an unknown exception");
            done->Run();
          }

          error = NO_ERROR;

        } else {

          error = INVALID_REQUEST;

        }

      } else {

        error = NO_METHOD;

      }

    } else {

      error = NO_SERVICE;

    }

  } else {

    error = NO_SERVICE;

  }



  if (error != NO_ERROR) {

    if (metrics_) {
      metrics_->RecordServerError();
    }

    RpcMessage response;

    response.set_type(RESPONSE);

    response.set_id(message->id());

    response.set_error(error);

    if (conn && conn->Connected()) {

      codec_.Send(conn, response);

    }

  }

}



void RpcChannel::OnServerRequestTimeout(
    const std::shared_ptr<ServerInflight>& inflight, int64_t id) {
  if (!inflight) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(inflight->mutex);
    if (inflight->response_sent) {
      return;
    }
    // 超时线程只改变响应状态，不接触仍可能由业务线程写入的响应对象。
    inflight->response_sent = true;
  }

  struct InflightGuard {
    std::atomic<int32_t>* counter;
    ~InflightGuard() { counter->fetch_sub(1, std::memory_order_relaxed); }
  } guard{&server_inflight_};

  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (!conn || !conn->Connected() ||
      closing_.load(std::memory_order_acquire)) {
    return;
  }

  RpcMessage message;
  message.set_type(RESPONSE);
  message.set_id(id);
  message.set_error(TIMEOUT);
  codec_.Send(conn, message);
}

void RpcChannel::DoneCallback(const std::shared_ptr<ServerInflight>& inflight,
                              int64_t id) {
  if (!inflight) {
    return;
  }
  bool send_response = false;
  std::shared_ptr<Timer> watchdog;
  std::unique_ptr<OwnedPendingResponse> owned;
  {
    std::lock_guard<std::mutex> lock(inflight->mutex);
    if (inflight->done) {
      return;
    }
    inflight->done = true;
    send_response = !inflight->response_sent;
    if (send_response) {
      inflight->response_sent = true;
    }
    watchdog = inflight->watchdog;
    if (inflight->pending != nullptr) {
      owned = std::make_unique<OwnedPendingResponse>(
          TakePending(inflight->pending));
      inflight->pending = nullptr;
    }
  }

  std::shared_ptr<TcpConnection> current_conn = ConnectionSnapshot();
  if (watchdog && current_conn && current_conn->GetLoop()) {
    current_conn->GetLoop()->CancelAfter(watchdog);
  }

  if (send_response) {
    server_inflight_.fetch_sub(1, std::memory_order_relaxed);
  }
  if (!send_response || !owned || !owned->pending) {
    return;
  }

  if (closing_.load(std::memory_order_acquire)) {
    return;
  }

  std::shared_ptr<TcpConnection> conn = ConnectionSnapshot();
  if (!conn || !conn->Connected()) {
    return;
  }

  RpcMessage message;
  message.set_type(RESPONSE);
  message.set_id(id);
  if (owned->controller != nullptr && owned->controller->Failed()) {
    message.set_error(INVALID_RESPONSE);
  } else {
    std::string response_payload;
    if (!owned->response->SerializeToString(&response_payload)) {
      message.set_error(INVALID_RESPONSE);
    } else {
      message.set_response(std::move(response_payload));
    }
  }
  codec_.Send(conn, message);
}



}  // namespace zrpc

