#include <atomic>
#include <chrono>
#include <thread>

#include "mapserver.pb.h"
#include "shared/map_world.h"
#include "shared/proto_conv.h"
#include "zrpc/base/logger.h"
#include "zrpc/grpc/rpc_client.h"
#include "zrpc/grpc/rpc_controller.h"

namespace {

constexpr uint64_t kEntityId = 1001;
constexpr float kDeltaTime = 1.0f / 60.0f;
constexpr int kSteps = 120;
constexpr float kTolerance = 0.05f;

class MapClientDemo {
 public:
  explicit MapClientDemo(zrpc::EventLoop* loop)
      : loop_(loop),
        rpc_client_(loop, "127.0.0.1", 9527),
        stub_(rpc_client_.channel()) {
    rpc_client_.SetConnectionCallback(
        std::bind(&MapClientDemo::OnConnected, this, std::placeholders::_1));
    rpc_client_.EnableRetry();
  }

  void Start() { rpc_client_.Connect(); }

 private:
  void OnConnected(const std::shared_ptr<zrpc::TcpConnection>& conn) {
    if (!conn->Connected()) {
      return;
    }
    if (started_.exchange(true)) {
      return;
    }
    LOG_INFO << "connected to mapserver, running shared front-backend demo";
    loop_->RunInLoop(std::bind(&MapClientDemo::RunScenario, this));
  }

  void RunScenario() {
  const mapserver::Vec3f spawn{0.0f, 3.0f, 0.0f};
  const mapserver::Vec3f impulse{2.0f, 0.0f, 1.5f};
  const float radius = 0.5f;

  if (!CreateEntityOnBothSides(spawn, radius)) {
    loop_->Quit();
    return;
  }
  if (!ApplyImpulseOnBothSides(impulse)) {
    loop_->Quit();
    return;
  }
  if (!StepOnBothSides(kSteps)) {
    loop_->Quit();
    return;
  }
  ValidateWithServer();
  loop_->Quit();
  }

  bool CreateEntityOnBothSides(const mapserver::Vec3f& pos, float radius) {
    if (!local_world_.CreateEntity(kEntityId, pos, radius)) {
      LOG_WARN << "local create entity failed";
      return false;
    }

    mapserver::CreateEntityRequest req;
    req.set_id(kEntityId);
    *req.mutable_position() = mapserver::ToProto(pos);
    req.set_radius(radius);

    mapserver::CreateEntityResponse resp;
    zrpc::RpcController ctrl;
    stub_.CreateEntity(&ctrl, &req, &resp, nullptr);
    if (ctrl.Failed() || !resp.ok()) {
      LOG_WARN << "remote create entity failed: " << ctrl.ErrorText() << " "
                << resp.message();
      return false;
    }
    LOG_INFO << "entity created on both client and server";
    return true;
  }

  bool ApplyImpulseOnBothSides(const mapserver::Vec3f& impulse) {
    if (!local_world_.ApplyImpulse(kEntityId, impulse)) {
      LOG_WARN << "local impulse failed";
      return false;
    }

    mapserver::ApplyImpulseRequest req;
    req.set_entity_id(kEntityId);
    *req.mutable_impulse() = mapserver::ToProto(impulse);

    mapserver::ApplyImpulseResponse resp;
    zrpc::RpcController ctrl;
    stub_.ApplyImpulse(&ctrl, &req, &resp, nullptr);
    if (ctrl.Failed() || !resp.ok()) {
      LOG_WARN << "remote impulse failed: " << ctrl.ErrorText() << " "
                << resp.message();
      return false;
    }
    LOG_INFO << "same impulse applied on both sides";
    return true;
  }

  bool StepOnBothSides(int steps) {
    for (int i = 0; i < steps; ++i) {
      local_world_.Step(kDeltaTime, 1);

      mapserver::StepRequest req;
      req.set_delta_time(kDeltaTime);
      req.set_collision_steps(1);

      mapserver::StepResponse resp;
      zrpc::RpcController ctrl;
      stub_.Step(&ctrl, &req, &resp, nullptr);
      if (ctrl.Failed()) {
        LOG_WARN << "remote step failed at i=" << i << ": "
                  << ctrl.ErrorText();
        return false;
      }
    }
    LOG_INFO << "simulated " << steps << " physics steps on both sides";
    return true;
  }

  void ValidateWithServer() {
    mapserver::WorldSnapshot local_snapshot;
    mapserver::FillSnapshot(local_world_.tick(), local_world_.Snapshot(),
                            &local_snapshot);

    mapserver::ValidateRequest req;
    *req.mutable_client_snapshot() = local_snapshot;
    req.set_tolerance(kTolerance);

    mapserver::ValidateResponse resp;
    zrpc::RpcController ctrl;
    stub_.Validate(&ctrl, &req, &resp, nullptr);
    if (ctrl.Failed()) {
      LOG_WARN << "validate rpc failed: " << ctrl.ErrorText();
      return;
    }

    const auto local_states = mapserver::FromSnapshot(local_snapshot);
  const auto server_states =
      mapserver::FromSnapshot(resp.server_snapshot());
  std::string local_diff;
  const bool local_match =
      local_world_.ValidateAgainst(server_states, kTolerance, &local_diff);

  LOG_INFO << "local compare match=" << local_match << " diff=" << local_diff;
  LOG_INFO << "server validate match=" << resp.match()
           << " diff=" << resp.diff();

  if (!local_states.empty() && !server_states.empty()) {
    const auto& a = local_states.front();
    const auto& b = server_states.front();
    LOG_INFO << "entity pos client=(" << a.position.x << "," << a.position.y
             << "," << a.position.z << ") server=(" << b.position.x << ","
             << b.position.y << "," << b.position.z << ")";
  }

  if (local_match && resp.match()) {
    LOG_INFO << "front-backend Jolt physics validation PASSED";
  } else {
    LOG_WARN << "front-backend Jolt physics validation FAILED";
  }
  }

  zrpc::EventLoop* loop_;
  zrpc::RpcClient rpc_client_;
  mapserver::MapService::Stub stub_;
  mapserver::MapWorld local_world_;
  std::atomic<bool> started_{false};
};

}  // namespace

int main() {
  zrpc::EventLoop loop;
  MapClientDemo demo(&loop);
  demo.Start();
  loop.Run();
  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}
