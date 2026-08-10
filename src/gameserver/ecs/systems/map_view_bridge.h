#pragma once

// MapViewBridge：地图生命周期网络包桥接层。
//
// 职责：订阅 EventBus 的地图生命周期事件（EvtEnterMap / EvtLeaveMap），
// 把地图层状态变化序列化为对应网络包发给客户端：
//   - EvtEnterMap → cli_3d_enter_map_ntf（地图配置/资源/出生点，通知客户端加载场景）
//   - EvtLeaveMap → cli_3d_leave_map_ntf（通知客户端卸载场景）
//
// 注：EvtMoveMap（跨格）属服务器内部 AOI 概念，无对应客户端协议，不在此桥接。
//
// 与 AoiViewBridge 区分：
//   - MapViewBridge 处理“地图层”事件（低频：进/离图）
//   - AoiViewBridge 处理“AOI 视野”事件（高频：appear/disappear/update）
// 两者各自独立订阅，职责不交叉。

#include <cstdint>
#include <functional>
#include <string>

#include "ecs/entity/entity.h"

class WorldSystem;

class MapViewBridge {
 public:
    using SendFn = std::function<void(uint64_t conn_owner_id, uint32_t msg_id,
                                       const std::string& body)>;

    MapViewBridge(WorldSystem* world, SendFn send_fn)
        : world_(world), send_fn_(std::move(send_fn)) {}

    ~MapViewBridge();

    MapViewBridge(const MapViewBridge&) = delete;
    MapViewBridge& operator=(const MapViewBridge&) = delete;

    void Install();

 private:
    void OnEntityEnterMap(const EntityPtr& entity);
    void OnEntityLeaveMap(const EntityPtr& entity);

    WorldSystem* world_;
    SendFn send_fn_;
    uint32_t enter_map_listener_id_ = 0;  // EventBus<EvtEnterMap> 订阅号
    uint32_t leave_map_listener_id_ = 0;  // EventBus<EvtLeaveMap> 订阅号
};
