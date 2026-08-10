#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

// 组件类型枚举；新增组件须同步枚举、IComponent 子类、DECLARE_COMPONENT。
enum class ComponentType : uint8_t {
  kNone     = 0,
  kConnection,
  kAccount,
  kRole,
  kTransform,
  kMap,
  kMove,
  kPlayerData,
};

class IComponent {
 public:
  virtual ~IComponent() = default;
  virtual ComponentType Type() const = 0;
};

template <typename T>
struct ComponentTraits;

#define DECLARE_COMPONENT(CompClass, EnumValue)       \
  template <>                                          \
  struct ComponentTraits<CompClass> {                  \
    static constexpr ComponentType kType = EnumValue;  \
  };

// 每实体每种 ComponentType 至多一份；非线程安全。
class ComponentStorage {
 public:
  template <typename T, typename... Args>
  T& Emplace(Args&&... args) {
    auto comp = std::make_unique<T>(std::forward<Args>(args)...);
    T& ref = *comp;
    components_[ComponentTraits<T>::kType] = std::move(comp);
    return ref;
  }

  template <typename T>
  T* Get() {
    auto it = components_.find(ComponentTraits<T>::kType);
    if (it == components_.end()) {
      return nullptr;
    }
    return static_cast<T*>(it->second.get());
  }

  template <typename T>
  const T* Get() const {
    auto it = components_.find(ComponentTraits<T>::kType);
    if (it == components_.end()) {
      return nullptr;
    }
    return static_cast<const T*>(it->second.get());
  }

  template <typename T>
  bool Has() const {
    return components_.find(ComponentTraits<T>::kType) != components_.end();
  }

  template <typename T>
  bool Remove() {
    return components_.erase(ComponentTraits<T>::kType) > 0;
  }

 private:
  std::unordered_map<ComponentType, std::unique_ptr<IComponent>> components_;
};
