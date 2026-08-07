# 寻路与碰撞异步线程设计

## 目标

寻路、碰撞检测、坐标合法性检测都可能比较耗时，尤其是大量实体同时移动、怪物批量寻路、客户端坐标同步校验时，不能直接阻塞主逻辑线程。

推荐做法是在引擎主线程启动后创建一个独立的 NavigationWorker 线程。主线程收到移动、寻路、碰撞请求后，把请求投递到异步队列；寻路线程计算完成后，把结果投递回主线程；主线程在 tick 中消费结果并更新游戏状态。

核心原则：

- 主线程负责收包、实体状态、地图状态、发包。
- 寻路线程只负责只读 navmesh 并计算结果。
- 寻路线程不能直接修改实体，也不能直接给客户端发包。
- 主线程消费结果时必须校验请求是否仍然有效。

## 整体流程

```text
网络/主逻辑线程
  收到移动/寻路/碰撞请求
  生成 NavRequest
  投递到 NavigationWorker 请求队列
        |
        v
寻路线程
  只读 navmesh
  执行 findPath / raycast / findNearestPoly / moveAlongSurface
  生成 NavResult
  投递到主线程结果队列
        |
        v
主线程 tick
  拉取 NavResult
  校验实体、地图、请求版本、时效
  更新服务端权威状态
  必要时同步客户端
```

## 请求结构

```cpp
enum class NavJobType
{
    FindPath,
    Raycast,
    ValidatePos,
    GetNearPos,
    MoveAlongSurface,
    GetHeight
};

struct NavRequest
{
    uint64_t requestId;
    uint64_t entityId;
    uint32_t mapId;
    int layer;
    NavJobType type;
    Position3D start;
    Position3D end;
    uint64_t submitTick;
};
```

字段说明：

- `requestId`：请求唯一 ID，用于结果回主线程后判断是否过期。
- `entityId`：发起请求的实体 ID。
- `mapId`：地图 ID，避免异步期间实体切图后误用旧结果。
- `layer`：navmesh layer。
- `type`：请求类型。
- `start/end`：查询坐标。
- `submitTick`：提交时刻，用于超时丢弃。

## 结果结构

```cpp
struct NavResult
{
    uint64_t requestId;
    uint64_t entityId;
    uint32_t mapId;
    NavJobType type;
    bool ok;
    int errorCode;
    std::vector<Position3D> points;
    Position3D hitPos;
    Position3D nearestPos;
};
```

字段说明：

- `ok`：查询是否成功。
- `errorCode`：失败原因，比如找不到最近 poly、路径为空、raycast 起点非法。
- `points`：路径点。
- `hitPos`：碰撞点。
- `nearestPos`：附近合法坐标或修正坐标。

## 主线程投递请求

主线程收到移动包或 AI 移动请求后，只负责构造请求并投递，不等待结果。

```cpp
void onMoveRequest(Entity& entity, const Position3D& target)
{
    NavRequest req;
    req.requestId = allocRequestId();
    req.entityId = entity.id();
    req.mapId = entity.mapId();
    req.layer = 0;
    req.type = NavJobType::FindPath;
    req.start = entity.position();
    req.end = target;
    req.submitTick = currentTick();

    entity.setLatestNavRequestId(req.requestId);
    navService.post(req);
}
```

注意：

- 不要在这里同步调用 `findStraightPath`。
- 同一实体连续投递请求时，只保留最新 `requestId`。
- 老请求可以不从队列中删除，但结果回来后必须丢弃。

## 寻路线程处理请求

寻路线程循环消费请求队列，执行 Detour 查询，然后把结果投递回主线程。

```cpp
void NavigationWorker::run()
{
    while (running_)
    {
        NavRequest req;
        if (!requestQueue_.waitPop(req))
            continue;

        NavResult result;
        result.requestId = req.requestId;
        result.entityId = req.entityId;
        result.mapId = req.mapId;
        result.type = req.type;

        executeRequest(req, result);

        resultQueue_.push(result);
    }
}
```

寻路线程只做计算：

- `FindPath`：`findNearestPoly` -> `findPath` -> `findStraightPath`
- `Raycast`：`findNearestPoly` -> `raycast`
- `ValidatePos`：`findNearestPoly` 并判断是否满足距离阈值
- `GetNearPos`：`findNearestPoly` 后返回 nearest point
- `MoveAlongSurface`：`findNearestPoly` -> `moveAlongSurface`
- `GetHeight`：`findNearestPoly` -> `getPolyHeight`

## 主线程消费结果

这段必须在主线程 tick 里处理。

```cpp
void GameTick()
{
    NavResult result;
    while (navService.tryPopResult(result))
    {
        if (!isRequestStillValid(result))
        {
            continue;
        }

        applyNavResult(result);
    }

    // 其他游戏逻辑...
}
```

`isRequestStillValid` 至少要检查：

- 实体是否还存在。
- 实体是否仍在同一张地图。
- `requestId` 是否仍然是实体最新请求。
- 请求是否超时。
- 实体是否死亡、传送、切场景、停止移动。

## applyNavResult 的职责

`applyNavResult` 不是单纯发包，它是主线程处理寻路结果的入口。

它通常负责：

- 更新服务端实体状态。
- 设置路径点。
- 停止移动。
- 坐标纠偏。
- 触发 AI 后续逻辑。
- 必要时给客户端发包。

示例：

```cpp
void applyNavResult(const NavResult& result)
{
    Entity* entity = findEntity(result.entityId);
    if (!entity)
        return;

    if (result.type == NavJobType::FindPath)
    {
        if (!result.ok)
        {
            entity->stopMove();
            sendMoveRejectToClient(*entity, result.errorCode);
            return;
        }

        entity->setPath(result.points);
        entity->setMoveState(MoveState::Moving);
        sendMovePathToClient(*entity, result.points);
    }
    else if (result.type == NavJobType::Raycast)
    {
        if (!result.ok)
        {
            entity->stopMove();
            sendMoveRejectToClient(*entity, result.errorCode);
        }
    }
    else if (result.type == NavJobType::GetNearPos)
    {
        if (result.ok)
        {
            entity->setPosition(result.nearestPos);
            sendCorrectPositionToClient(*entity, result.nearestPos);
        }
    }
}
```

原则是先更新服务端权威状态，再按需要同步客户端。

## Detour 线程安全约束

`dtNavMesh` 可以作为只读数据共享，但不能在 worker 查询期间被释放或重载。

`dtNavMeshQuery` 不建议多线程共享，因为内部包含查询用的临时状态，比如 node pool 和 open list。推荐做法：

- 每个 NavigationWorker 持有自己的 `dtNavMeshQuery`。
- 多 worker 时，每个 worker 一套 query。
- 不要所有线程共用同一个 `dtNavMeshQuery`。

navmesh 生命周期建议：

- 加载后作为只读对象使用。
- reload 时使用双缓冲或引用计数。
- 新请求使用新 navmesh。
- 老请求跑完后再释放旧 navmesh。

## 队列与限流

异步化后必须控制队列压力，否则高峰时请求堆积会导致延迟越来越大。

建议策略：

- 每 tick 限制每个实体最多提交一个最新寻路请求。
- 同一实体旧请求结果直接丢弃。
- 全局请求队列设置最大长度。
- 队列满时优先丢弃低优先级请求，比如普通怪物远距离重规划。
- 玩家请求优先级高于普通 AI。
- 碰撞检测、坐标校验这种小请求可以批量处理。

优先级建议：

```text
玩家移动纠偏 > 玩家寻路 > 战斗碰撞 > Boss AI > 普通怪物 AI > 巡逻/闲逛
```

## 哪些适合异步

适合异步：

- 长距离寻路。
- 大量怪物批量寻路。
- 客户端坐标同步校验。
- 复杂 raycast。
- 大量 `GetNearPos` 修正。
- AI 重规划。

不一定适合异步：

- 每 tick 极小量的简单坐标判断。
- 必须立即决定当前帧结果的逻辑。
- 调用频率很低的单次查询。

如果一个查询非常轻，投递线程队列的成本可能比直接算更高。建议先把重查询异步化，轻查询保留同步或批量处理。

## 推荐落地顺序

1. 先实现单线程 `NavigationWorker`。
2. 请求队列和结果队列打通。
3. 主线程只投递请求，不等待结果。
4. 主线程 tick 消费结果。
5. 加入 `requestId` 过期丢弃。
6. 加入 mapId/entityId 校验。
7. 加入队列长度限制和优先级。
8. 压测后再决定是否扩展成多个 worker。

不建议一开始就做线程池。单 worker 更容易保证 navmesh 生命周期、请求顺序和问题定位。

## 与当前工程的对应关系

当前工程里的同步接口：

- `NavMeshHandle::findStraightPath`
- `NavMeshHandle::raycast`
- `NavMeshHandle::raycastNear`
- `NavMeshHandle::raycastAlong`
- `NavMeshHandle::GetHeight`
- `NavMeshHandle::GetNearPos`
- `NavMeshHandle::Intersects`

异步线程内部可以复用这些能力，但需要注意 query 对象不要跨线程共享。后续可以在 `NavigationService` 内部按 mapId 持有 navmesh 数据，并给每个 worker 初始化自己的 query。

## 总结

推荐架构是：

```text
主线程投递请求
寻路线程只读 navmesh 计算
主线程消费结果并修改实体/发包
```

这能把耗时寻路和碰撞查询从主逻辑线程剥离，同时避免多线程直接改游戏对象带来的竞态问题。
