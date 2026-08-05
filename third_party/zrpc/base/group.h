#pragma once

#include <algorithm>
#include <any>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class Context;
using HandleAction = std::function<void(Context&)>;
using Values = std::unordered_map<std::string, std::any>;

class Context {
 public:
  Context(int64_t id, Values values)
      : id_(id), index_(0), values_(std::move(values)) {}

  Context(int64_t id, Values values, std::vector<HandleAction> handlers)
      : id_(id),
        index_(0), values_(std::move(values)),
        handlers_(std::move(handlers)) {}

  int64_t Id() const noexcept { return id_; }

  void Abort() noexcept { index_ = handlers_.size(); }
  void Next() {
    ++index_;
    while (index_ < handlers_.size() && !handlers_[index_]) {
      ++index_;
    }
    if (index_ < handlers_.size()) {
      handlers_[index_](*this);
    }
  }

  void Do() {
    while (index_ < handlers_.size()) {
      if (handlers_[index_]) {
        handlers_[index_](*this);
      }
      ++index_;
    }
  }

  Context& Set(std::string key, std::any value) {
    values_[std::move(key)] = std::move(value);
    return *this;
  }

  template <class T>
  std::optional<T> Get(std::string_view key) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end() || !it->second.has_value()) {
      return std::nullopt;
    }

    if (const auto* p = std::any_cast<T>(&it->second)) {
      return *p;
    }
    return std::nullopt;
  }

  template <class T>
  T MustGet(std::string_view key) const {
    auto it = values_.find(std::string(key));
    if (it == values_.end() || !it->second.has_value()) {
      throw std::runtime_error("Context.MustGet: missing key: " +
                               std::string(key));
    }
    return std::any_cast<T>(it->second);
  }

  const Values& ValuesRef() const noexcept { return values_; }
  Values& ValuesRef() noexcept { return values_; }

 private:
  int64_t id_;
  std::size_t index_;
  Values values_;
  std::vector<HandleAction> handlers_;
};

class Engine;

class Group {
 public:
  Group(Engine* root, std::vector<HandleAction>&& middlewares)
      : root_(root), middlewares_(std::move(middlewares)) {}

  Group GroupWith(const std::vector<HandleAction>& more_middlewares) const;

  Group& Use(const std::vector<HandleAction>& handlers) {
    if (std::any_of(handlers.begin(), handlers.end(),
                    [](const HandleAction& handler) { return !handler; })) {
      throw std::invalid_argument("Group middleware must not be empty");
    }
    middlewares_.insert(middlewares_.end(), handlers.begin(), handlers.end());
    return *this;
  }

  Group& Use(std::vector<HandleAction>&& handlers) {
    if (std::any_of(handlers.begin(), handlers.end(),
                    [](const HandleAction& handler) { return !handler; })) {
      throw std::invalid_argument("Group middleware must not be empty");
    }
    middlewares_.insert(middlewares_.end(),
                        std::make_move_iterator(handlers.begin()),
                        std::make_move_iterator(handlers.end()));
    return *this;
  }

  Group& Register(int64_t pid, HandleAction handler);

 private:
  Engine* root_;
  std::vector<HandleAction> middlewares_;
};

class Engine {
 public:
  Engine() : root_group_(this, {}) {}

  Group& Root() noexcept { return root_group_; }

  void RegisterInternal(int64_t pid, std::vector<HandleAction>&& handlers) {
    std::unique_lock<std::shared_mutex> lk(handlers_mutex_);
    handlers_[pid] = std::move(handlers);
  }

  void Call(int64_t pid, Values values, HandleAction handler) {
    std::vector<HandleAction> chain;
    {
      std::shared_lock<std::shared_mutex> lk(handlers_mutex_);
      auto it = handlers_.find(pid);
      if (it != handlers_.end()) {
        chain = it->second;
      }
    }

    bool replaced = false;
    if (handler) {
      for (auto& h : chain) {
        if (!h) {
          h = std::move(handler);
          replaced = true;
          break;
        }
      }

      if (!replaced) {
        chain.push_back(std::move(handler));
      }
    }

    Context ctx(pid, std::move(values), std::move(chain));
    ctx.Do();
  }

  void Do(int64_t pid, Values values) {
    std::vector<HandleAction> chain;
    {
      std::shared_lock<std::shared_mutex> lk(handlers_mutex_);
      auto it = handlers_.find(pid);
      if (it != handlers_.end()) {
        chain = it->second;
      }
    }
    Context ctx(pid, std::move(values), std::move(chain));
    ctx.Do();
  }

 private:
  friend class Group;

  Group root_group_;
  mutable std::shared_mutex handlers_mutex_;
  std::unordered_map<int64_t, std::vector<HandleAction>> handlers_;
};

inline Group Group::GroupWith(
    const std::vector<HandleAction>& more_middlewares) const {
  std::vector<HandleAction> merged;
  merged.reserve(middlewares_.size() + more_middlewares.size());
  merged.insert(merged.end(), middlewares_.begin(), middlewares_.end());
  merged.insert(merged.end(),
                std::make_move_iterator(more_middlewares.begin()),
                std::make_move_iterator(more_middlewares.end()));
  return Group(root_, std::move(merged));
}

inline Group& Group::Register(int64_t pid, HandleAction handler) {
  if (!handler) {
    throw std::invalid_argument("Group handler must not be empty");
  }
  std::vector<HandleAction> chain;
  chain.reserve(middlewares_.size() + 1);
  chain.insert(chain.end(), middlewares_.begin(), middlewares_.end());
  chain.push_back(std::move(handler));
  root_->RegisterInternal(pid, std::move(chain));
  return *this;
}
