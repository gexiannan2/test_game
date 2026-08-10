#pragma once

#include <google/protobuf/service.h>

#include <type_traits>
#include <utility>

namespace zrpc {

template <typename Fn>
class RpcCallbackFn : public ::google::protobuf::Closure {
 public:
  explicit RpcCallbackFn(Fn fn) : fn_(std::move(fn)) {}

  void Run() override {
    Fn fn = std::move(fn_);
    delete this;
    fn();
  }

 private:
  Fn fn_;
};

template <typename Fn>
inline ::google::protobuf::Closure* NewRpcCallback(Fn&& fn) {
  using F = std::decay_t<Fn>;
  return new RpcCallbackFn<F>(std::forward<Fn>(fn));
}

}  // namespace zrpc
