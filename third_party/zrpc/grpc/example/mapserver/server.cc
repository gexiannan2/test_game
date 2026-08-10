#include <memory>
#include <mutex>

#include "mapserver.pb.h"
#include "shared/map_world.h"
#include "shared/proto_conv.h"
#include "zrpc/base/logger.h"
#include "zrpc/grpc/rpc_server.h"

namespace {

class MapServiceImpl final : public ::mapserver::MapService {
 public:
  void CreateEntity(::google::protobuf::RpcController* controller,
                    const ::mapserver::CreateEntityRequest* request,
                    ::mapserver::CreateEntityResponse* response,
                    ::google::protobuf::Closure* done) override {
    (void)controller;
    std::lock_guard<std::mutex> lk(mu_);
    const bool ok = world_.CreateEntity(
        request->id(), mapserver::FromProto(request->position()),
        request->radius());
    response->set_ok(ok);
    response->set_message(ok ? "created" : "create failed");
    done->Run();
  }

  void ApplyImpulse(::google::protobuf::RpcController* controller,
                    const ::mapserver::ApplyImpulseRequest* request,
                    ::mapserver::ApplyImpulseResponse* response,
                    ::google::protobuf::Closure* done) override {
    (void)controller;
    std::lock_guard<std::mutex> lk(mu_);
    const bool ok = world_.ApplyImpulse(request->entity_id(),
                                      mapserver::FromProto(request->impulse()));
    response->set_ok(ok);
    response->set_message(ok ? "impulse applied" : "impulse failed");
    done->Run();
  }

  void Step(::google::protobuf::RpcController* controller,
            const ::mapserver::StepRequest* request,
            ::mapserver::StepResponse* response,
            ::google::protobuf::Closure* done) override {
    (void)controller;
    std::lock_guard<std::mutex> lk(mu_);
    world_.Step(request->delta_time(), request->collision_steps());
    mapserver::FillSnapshot(world_.tick(), world_.Snapshot(),
                            response->mutable_snapshot());
    done->Run();
  }

  void GetSnapshot(::google::protobuf::RpcController* controller,
                   const ::mapserver::GetSnapshotRequest* request,
                   ::mapserver::StepResponse* response,
                   ::google::protobuf::Closure* done) override {
    (void)controller;
    (void)request;
    std::lock_guard<std::mutex> lk(mu_);
    mapserver::FillSnapshot(world_.tick(), world_.Snapshot(),
                            response->mutable_snapshot());
    done->Run();
  }

  void Validate(::google::protobuf::RpcController* controller,
                const ::mapserver::ValidateRequest* request,
                ::mapserver::ValidateResponse* response,
                ::google::protobuf::Closure* done) override {
    (void)controller;
    std::lock_guard<std::mutex> lk(mu_);
    const auto client_states =
        mapserver::FromSnapshot(request->client_snapshot());
    std::string diff;
    const bool match = world_.ValidateAgainst(
        client_states, request->tolerance(), &diff);
    response->set_match(match);
    response->set_diff(diff);
    mapserver::FillSnapshot(world_.tick(), world_.Snapshot(),
                            response->mutable_server_snapshot());
    done->Run();
  }

 private:
  mutable std::mutex mu_;
  mapserver::MapWorld world_;
};

}  // namespace

int main() {
  MapServiceImpl service;
  zrpc::EventLoop loop;
  zrpc::RpcServer server(&loop, "0.0.0.0", 9527);
  server.RegisterService(&service);
  server.Start();
  LOG_INFO << "mapserver listening on 0.0.0.0:9527";
  loop.Run();
  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}
