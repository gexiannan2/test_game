#pragma once

// AOI/Map 事件 → 网络包桥接（appear/disappear/update/enter_map）。

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ecs/entity/entity.h"

class WorldSystem;

class AoiViewBridge {
 public:
    using SendFn = std::function<void(uint64_t conn_owner_id, uint32_t msg_id,
                                      const std::string& body)>;

    AoiViewBridge(WorldSystem* world, SendFn send_fn);
    ~AoiViewBridge();

    void Install();

    bool BuildAppearBody(const EntityPtr& observee, const EntityPtr& observer,
                        std::string& out_body);
    bool BuildDisappearBody(const EntityPtr& observee, std::string& out_body);

 private:
    void OnEntityAppear(uint64_t viewer_id, uint64_t subject_id);
    void OnEntityDisappear(uint64_t viewer_id, uint64_t subject_id);
    void OnEntityUpdate(uint64_t viewer_id, uint64_t subject_id);
    void OnEntityEnterMap(const EntityPtr& entity);
    void OnBroadcast(const AoiBroadcastEvent& ev);

    WorldSystem* world_;
    SendFn send_fn_;
};
