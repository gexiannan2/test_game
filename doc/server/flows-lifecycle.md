<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 13. 从 0 到 1 全生命周期

```mermaid
sequenceDiagram
    participant C as 客户端
    participant GS as GameServer
    participant H as Handlers
    participant SYS as System
    participant WS as WorldSystem
    participant AOI as AoiSystem
    participant BR as AoiViewBridge

    C->>GS: TCP connect
    GS->>GS: PlayerEntity + ConnectionComponent
    C->>H: handshake → login → role_login
    H->>SYS: 写组件 / 状态机
    C->>H: enter_game
    H->>WS: EnterMap
    WS->>AOI: OnEntityIntoMap + AddWatcher + NotifySelfAppear
    AOI->>BR: 别人看到你 / 你看到别人 / 自身 appear
    BR->>C: appear / enter_map
    loop 游戏中
        C->>H: move_req
        H->>WS: MoveEntity → OnMove
        WS->>AOI: 跨格 SwitchMonitor / MoveWatcher
        AOI->>BR: appear/disappear
        H->>C: move_res(自己)
    end
    C--xGS: 断线/心跳/踢人
    GS->>WS: LeaveMap
    AOI->>BR: disappear 给周围
```

逐步文字版：

1. **启动** → 配置、世界、桥接、Handler、听端口、心跳、Tick  
2. **连接** → 临时实体  
3. **握手/登录/选角** → PES 缓存，状态走到 kRoleSelected  
4. **enter_game** → EnterMap + 双向视野 + 自身 appear  
5. **move** → MoveEntity 跨格才 AOI 广播  
6. **断线** → LeaveMap，实体留 PES  
7. **重连** → session 校验 → EnterMap 重建视野  

---
