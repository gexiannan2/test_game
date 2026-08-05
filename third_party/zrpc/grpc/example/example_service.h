#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "sudoku.pb.h"
#include "zrpc/base/logger.h"
#include "zrpc/grpc/rpc.pb.h"
#include "zrpc/grpc/rpc_controller.h"

namespace rpc_example {

enum class ServiceMode {
  kNormal = 0,
  kFail,
  kSlow,
  kAsync,
  kInvalidResponse,
};

inline const char* ServiceModeName(ServiceMode mode) {
  switch (mode) {
    case ServiceMode::kNormal:
      return "normal";
    case ServiceMode::kFail:
      return "fail";
    case ServiceMode::kSlow:
      return "slow";
    case ServiceMode::kAsync:
      return "async";
    case ServiceMode::kInvalidResponse:
      return "invalid_response";
    default:
      return "unknown";
  }
}

inline ServiceMode ParseServiceMode(const std::string& name) {
  if (name == "fail") {
    return ServiceMode::kFail;
  }
  if (name == "slow") {
    return ServiceMode::kSlow;
  }
  if (name == "async") {
    return ServiceMode::kAsync;
  }
  if (name == "invalid_response") {
    return ServiceMode::kInvalidResponse;
  }
  return ServiceMode::kNormal;
}

class SudokuServiceImpl : public sudoku::SudokuService {
 public:
  explicit SudokuServiceImpl(ServiceMode mode = ServiceMode::kNormal)
      : mode_(mode) {}

  void set_mode(ServiceMode mode) { mode_ = mode; }
  ServiceMode mode() const { return mode_; }
  int handled() const { return handled_.load(std::memory_order_relaxed); }

  void Solve(::google::protobuf::RpcController* controller,
             const sudoku::SudokuRequest* request,
             sudoku::SudokuResponse* response,
             ::google::protobuf::Closure* done) override {
    handled_.fetch_add(1, std::memory_order_relaxed);

    if (request->checkerboard().empty()) {
      controller->SetFailed("empty checkerboard");
      if (auto* ctrl = dynamic_cast<zrpc::RpcController*>(controller)) {
        ctrl->SetErrorCode(static_cast<int>(INVALID_REQUEST));
      }
      done->Run();
      return;
    }

    switch (mode_) {
      case ServiceMode::kFail:
        controller->SetFailed("injected service failure");
        if (auto* ctrl = dynamic_cast<zrpc::RpcController*>(controller)) {
          ctrl->SetErrorCode(static_cast<int>(INVALID_RESPONSE));
        }
        done->Run();
        return;
      case ServiceMode::kSlow:
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        break;
      case ServiceMode::kAsync:
        std::thread([request, response, done]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          response->set_solved(true);
          response->set_checkerboard(request->checkerboard());
          done->Run();
        }).detach();
        return;
      case ServiceMode::kInvalidResponse:
        response->set_solved(false);
        response->set_checkerboard("not-a-valid-board");
        done->Run();
        return;
      case ServiceMode::kNormal:
      default:
        break;
    }

    response->set_solved(true);
    response->set_checkerboard("123456789");
    done->Run();
  }

 private:
  ServiceMode mode_;
  std::atomic<int> handled_{0};
};

}  // namespace rpc_example
