// AOI 协议级单机测试：appear/disappear/脏同步，ProtoCapture 替代网络发送。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client_3d.pb.h"  // cli_3d_aoi_appears_ntf / cli_3d_aoi_disappears_ntf / ENTITY_PLAYER
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "ecs/entity/player_entity.h"
#include "protocol/pack_flags.h"
#include "common/aoi_def.h"
#include "common/event_bus.h"
#include "ecs/systems/aoi_system.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/world_system.h"
#include "test_harness.h"

namespace {

// kGridSize=4, kAoiRadius=0：同 AOI 格内可见。kNear* 同格，kFar* 跨格不可见。
constexpr float kNearX = 1.0f, kNearZ = 1.0f;
constexpr float kFarX = 1000.0f, kFarZ = 1000.0f;

// ---- 捕获帧 ----
struct CapturedFrame {
    uint64_t observer_id = 0;  // 观察者实体 ID
    uint32_t msg_id = 0;       // 协议号
    std::string body;          // 序列化后的包体
};

// ProtoCapture：复刻 AoiViewBridge 序列化路径，捕获帧替代网络发送。
class ProtoCapture {
 public:
    std::vector<CapturedFrame> frames;

    ~ProtoCapture() {
        if (enter_map_listener_id_ != 0) {
            EventBus::Instance().Unsubscribe<EvtEnterMap>(enter_map_listener_id_);
            enter_map_listener_id_ = 0;
        }
    }

    void Bind(WorldSystem* world) {
        world_ = world;
        auto& aoi = world->Aoi();
        aoi.SetEntityEnterCallback(
            [this](uint64_t viewer, const std::vector<uint64_t>& subjects) {
                for (uint64_t sid : subjects) OnAppear(viewer, sid);
            });
        aoi.SetEntityLeaveCallback(
            [this](uint64_t viewer, const std::vector<uint64_t>& subjects) {
                for (uint64_t sid : subjects) OnDisappear(viewer, sid);
            });
        aoi.SetEntityUpdateCallback(
            [this](uint64_t viewer, uint64_t subject) { OnUpdate(viewer, subject); });
        // 进图事件改走 EventBus（与 MapViewBridge::OnEntityEnterMap 一致）
        enter_map_listener_id_ = EventBus::Instance().Subscribe<EvtEnterMap>(
            [this](const EvtEnterMap& ev) {
                if (ev.entity) OnEnterMap(ev.entity);
            });
    }

    void Clear() { frames.clear(); }

    // 统计某观察者收到的指定 msg_id 帧数
    size_t CountFrames(uint64_t observer_id, uint32_t msg_id) const {
        size_t n = 0;
        for (const auto& f : frames) {
            if (f.observer_id == observer_id && f.msg_id == msg_id) ++n;
        }
        return n;
    }

    // 取某观察者收到的 appears_ntf 中，list(0).entity_id() == expected 的第一帧
    const CapturedFrame* FindAppearFrame(uint64_t observer_id, uint64_t expected_entity_id) const {
        for (const auto& f : frames) {
            if (f.observer_id != observer_id) continue;
            if (f.msg_id != proto_id("cli_3d_aoi_appears_ntf")) continue;
            ::cli_3d_aoi_appears_ntf appear;
            if (!appear.ParseFromString(f.body)) continue;
            if (appear.list_size() >= 1 && appear.list(0).entity_id() == expected_entity_id) {
                return &f;
            }
        }
        return nullptr;
    }

    // 取某观察者收到的指定 msg_id 的第一帧（找不到返回 nullptr）
    const CapturedFrame* FirstFrame(uint64_t observer_id, uint32_t msg_id) const {
        for (const auto& f : frames) {
            if (f.observer_id == observer_id && f.msg_id == msg_id) return &f;
        }
        return nullptr;
    }

 private:
    WorldSystem* world_ = nullptr;
    uint32_t enter_map_listener_id_ = 0;  // EventBus<EvtEnterMap> 订阅号

    void OnAppear(uint64_t viewer_id, uint64_t subject_id) {
        EntityPtr observer = world_->FindEntity(viewer_id);
        EntityPtr observee = world_->FindEntity(subject_id);
        if (!observer || !observee) return;
        SerializeMsg sm;
        if (!observee->SerializeAppear(sm, observer)) return;
        if (!sm.msg) return;
        std::string body;
        if (!sm.msg->SerializeToString(&body) || body.empty()) return;
        frames.push_back({viewer_id, sm.msg_id, std::move(body)});
    }

    void OnDisappear(uint64_t viewer_id, uint64_t subject_id) {
        EntityPtr observee = world_->FindEntity(subject_id);
        if (!observee) return;
        auto* role = observee->GetComponent<RoleComponent>();
        ::cli_3d_aoi_disappears_ntf disappear;
        disappear.add_entity_id_list(role ? role->role_id_ : observee->GetId());
        std::string body;
        if (!disappear.SerializeToString(&body) || body.empty()) return;
        frames.push_back({viewer_id, proto_id("cli_3d_aoi_disappears_ntf"), std::move(body)});
    }

    void OnUpdate(uint64_t viewer_id, uint64_t subject_id) {
        EntityPtr observee = world_->FindEntity(subject_id);
        if (!observee) return;
        std::vector<SerializeMsg> msgs;
        if (!observee->SerializeDirty(msgs)) return;
        for (SerializeMsg& sm : msgs) {
            if (!sm.msg) continue;
            std::string body;
            if (!sm.msg->SerializeToString(&body) || body.empty()) continue;
            frames.push_back({viewer_id, sm.msg_id, std::move(body)});
        }
    }

    void OnEnterMap(const EntityPtr& entity) {
        if (!entity) return;
        // 玩家自己收到 cli_3d_enter_map_ntf（与 MapViewBridge::OnEntityEnterMap 一致）
        ::cli_3d_enter_map_ntf enter_map;
        enter_map.set_cfg_id(0);
        enter_map.set_map_id(0);
        enter_map.set_source_id("map_default");
        auto* role = entity->GetComponent<RoleComponent>();
        enter_map.set_role_entity_id(role ? role->role_id_ : entity->GetId());
        enter_map.set_err_code(0);
        std::string body;
        if (!enter_map.SerializeToString(&body) || body.empty()) return;
        frames.push_back({entity->GetId(), proto_id("cli_3d_enter_map_ntf"), std::move(body)});
    }
};

struct ProtoTestWorld {
    std::shared_ptr<WorldSystem> world;
    ProtoCapture capture;

    ProtoTestWorld() {
        world = WorldSystem::Create(SceneRegionType::kMap);
        world->Init();
        world->SetEntityFactory(
            [](WorldSystem* w, uint64_t id, EntityType type,
               const EntitySpawn& spawn) -> EntityPtr {
                auto e = std::make_shared<PlayerEntity>(w, id, type, spawn);
                e->AddComponent<RoleComponent>();
                auto* role = e->GetComponent<RoleComponent>();
                role->role_id_ = id * 100;  // role_id 与 entity_id 区分开，便于断言
                role->name_ = "player_" + std::to_string(id);
                role->level_ = 1;
                role->job_ = 1;
                role->sex_ = 1;
                return e;
            });
        capture.Bind(world.get());
    }
};

// 在指定位置生成一个玩家并进图
EntityPtr SpawnPlayer(ProtoTestWorld& tw, float x, float z) {
    EntitySpawn spawn;
    spawn.position = JPH::Vec3(x, 0.0f, z);
    return tw.world->SpawnOnMap(EntityType::kPlayer, spawn);
}

}  // namespace

GAME_TEST_SUITE(AoiProtoTest);

GAME_TEST(AoiProtoTest, EnterMapAppearBroadcast) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    auto b = SpawnPlayer(tw, kNearX + 1.0f, kNearZ + 1.0f);
    EXPECT_TRUE(a && b);
    tw.world->Tick();  // 触发 FlushDirty（进图不标脏，但保证一致性）

    const uint64_t a_role = a->GetComponent<RoleComponent>()->role_id_;
    const uint64_t b_role = b->GetComponent<RoleComponent>()->role_id_;

    // A 应收到 B 的 appear（跳过 A 自身的 self-appear 帧）
    const CapturedFrame* fa = tw.capture.FindAppearFrame(a->GetId(), b_role);
    EXPECT_TRUE(fa != nullptr);
    if (fa) {
        ::cli_3d_aoi_appears_ntf appear;
        EXPECT_TRUE(appear.ParseFromString(fa->body));
        EXPECT_GE(appear.list_size(), 1);
        if (appear.list_size() >= 1) {
            EXPECT_EQ(appear.list(0).entity_id(), b_role);  // B 的 role_id
            EXPECT_EQ(appear.list(0).type(), ::ENTITY_PLAYER);
        }
    }

    // B 应收到 A 的 appear（跳过 B 自身的 self-appear 帧）
    const CapturedFrame* fb = tw.capture.FindAppearFrame(b->GetId(), a_role);
    EXPECT_TRUE(fb != nullptr);
    if (fb) {
        ::cli_3d_aoi_appears_ntf appear;
        EXPECT_TRUE(appear.ParseFromString(fb->body));
        EXPECT_GE(appear.list_size(), 1);
        if (appear.list_size() >= 1) {
            EXPECT_EQ(appear.list(0).entity_id(), a_role);  // A 的 role_id
            EXPECT_EQ(appear.list(0).type(), ::ENTITY_PLAYER);
        }
    }
}

GAME_TEST(AoiProtoTest, EnterMapSelfReceivesEnterMap) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    EXPECT_TRUE(a);
    tw.world->Tick();

    const uint64_t a_role = a->GetComponent<RoleComponent>()->role_id_;
    const CapturedFrame* f = tw.capture.FirstFrame(a->GetId(), proto_id("cli_3d_enter_map_ntf"));
    EXPECT_TRUE(f != nullptr);
    if (f) {
        ::cli_3d_enter_map_ntf em;
        EXPECT_TRUE(em.ParseFromString(f->body));
        EXPECT_EQ(em.role_entity_id(), a_role);
        EXPECT_EQ(em.err_code(), 0);
    }
}

GAME_TEST(AoiProtoTest, LeaveMapDisappearBroadcast) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    auto b = SpawnPlayer(tw, kNearX + 1.0f, kNearZ + 1.0f);
    EXPECT_TRUE(a && b);
    tw.capture.Clear();  // 清掉进图帧，只看离图
    tw.world->Tick();

    const uint64_t b_role = b->GetComponent<RoleComponent>()->role_id_;
    tw.world->LeaveMap(b);
    tw.world->Tick();

    tw.world->Tick();

    const CapturedFrame* f = tw.capture.FirstFrame(a->GetId(), proto_id("cli_3d_aoi_disappears_ntf"));
    EXPECT_TRUE(f != nullptr);
    if (f) {
        ::cli_3d_aoi_disappears_ntf disappear;
        EXPECT_TRUE(disappear.ParseFromString(f->body));
        EXPECT_GE(disappear.entity_id_list_size(), 1);
        if (disappear.entity_id_list_size() >= 1) {
            EXPECT_EQ(disappear.entity_id_list(0), b_role);
        }
    }

    // B 离图后不应再收到任何 AOI 广播
    EXPECT_EQ(tw.capture.CountFrames(b->GetId(), proto_id("cli_3d_aoi_appears_ntf")), 0u);
}

GAME_TEST(AoiProtoTest, DirtySyncBroadcastMove) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    auto b = SpawnPlayer(tw, kNearX + 1.0f, kNearZ + 1.0f);
    EXPECT_TRUE(a && b);
    tw.capture.Clear();
    tw.world->Tick();

    const uint64_t a_role = a->GetComponent<RoleComponent>()->role_id_;

    // 同 AOI 格内移动（勿跨 10m 视野格）
    tw.world->MoveEntity(a, JPH::Vec3(5.0f, 0.0f, 5.0f), EntityPropertyType::kMove);
    a->SetPropertyDirty(EntityPropertyType::kMove);
    tw.world->Tick();  // FlushDirty → AOI update → SerializeDirty

    const CapturedFrame* f = tw.capture.FirstFrame(b->GetId(), proto_id("cli_3d_aoi_appears_ntf"));
    EXPECT_TRUE(f != nullptr);
    if (f) {
        ::cli_3d_aoi_appears_ntf appear;
        EXPECT_TRUE(appear.ParseFromString(f->body));
        EXPECT_GE(appear.list_size(), 1);
        if (appear.list_size() >= 1) {
            const auto& e3d = appear.list(0);
            EXPECT_EQ(e3d.entity_id(), a_role);
            EXPECT_EQ(e3d.type(), ::ENTITY_PLAYER);
            // 在 entity_data_list 中查找 PLAYER_DATA 以读取 base.pos
            bool pos_checked = false;
            for (const auto& ed : e3d.entity_data_list()) {
                if (ed.type() != ::ENTITY_DATA_TYPE_PLAYER_DATA) continue;
                ::entity_player_data pd;
                if (!pd.ParseFromString(ed.data())) continue;
                if (!pd.has_base()) continue;
                EXPECT_NEAR(pd.base().pos().x(), 5.0f, 0.01f);
                EXPECT_NEAR(pd.base().pos().z(), 5.0f, 0.01f);
                pos_checked = true;
                break;
            }
            EXPECT_TRUE(pos_checked);
        }
    }
}

GAME_TEST(AoiProtoTest, DirtySyncBroadcastStopMove) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    auto b = SpawnPlayer(tw, kNearX + 1.0f, kNearZ + 1.0f);
    EXPECT_TRUE(a && b);
    // 给 A 设一个非零速度，模拟移动中
    a->SetVelocity(JPH::Vec3(5.0f, 0.0f, 3.0f), EntityPropertyType::kMove);
    tw.capture.Clear();
    tw.world->Tick();

    // 标记 kStopMove：move_handler 停止时 velocity 已清零，这里模拟清零后标脏
    a->SetVelocity(JPH::Vec3::sZero(), EntityPropertyType::kMove);
    a->SetPropertyDirty(EntityPropertyType::kStopMove);
    tw.world->Tick();

    const CapturedFrame* f = tw.capture.FirstFrame(b->GetId(), proto_id("cli_3d_aoi_appears_ntf"));
    EXPECT_TRUE(f != nullptr);
    if (f) {
        ::cli_3d_aoi_appears_ntf appear;
        EXPECT_TRUE(appear.ParseFromString(f->body));
        EXPECT_GE(appear.list_size(), 1);
        if (appear.list_size() >= 1) {
            const auto& e3d = appear.list(0);
            // 在 entity_data_list 中查找 PLAYER_DATA 以读取 base.velocity
            bool vel_checked = false;
            for (const auto& ed : e3d.entity_data_list()) {
                if (ed.type() != ::ENTITY_DATA_TYPE_PLAYER_DATA) continue;
                ::entity_player_data pd;
                if (!pd.ParseFromString(ed.data())) continue;
                if (!pd.has_base()) continue;
                // 停止后 velocity 应为 0
                EXPECT_NEAR(pd.base().velocity().x(), 0.0f, 0.01f);
                EXPECT_NEAR(pd.base().velocity().y(), 0.0f, 0.01f);
                EXPECT_NEAR(pd.base().velocity().z(), 0.0f, 0.01f);
                vel_checked = true;
                break;
            }
            EXPECT_TRUE(vel_checked);
        }
    }
}

GAME_TEST(AoiProtoTest, MoveCrossGridDisappearAndReappear) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    auto b = SpawnPlayer(tw, kNearX + 1.0f, kNearZ + 1.0f);
    EXPECT_TRUE(a && b);
    tw.capture.Clear();
    tw.world->Tick();

    const uint64_t b_role = b->GetComponent<RoleComponent>()->role_id_;

    // B 移到远处（跨 AOI 格）→ A 收到 disappear
    tw.world->MoveEntity(b, JPH::Vec3(kFarX, 0.0f, kFarZ), EntityPropertyType::kMove);
    tw.world->Tick();

    const CapturedFrame* df = tw.capture.FirstFrame(a->GetId(), proto_id("cli_3d_aoi_disappears_ntf"));
    EXPECT_TRUE(df != nullptr);
    if (df) {
        ::cli_3d_aoi_disappears_ntf disappear;
        EXPECT_TRUE(disappear.ParseFromString(df->body));
        EXPECT_GE(disappear.entity_id_list_size(), 1);
        if (disappear.entity_id_list_size() >= 1) {
            EXPECT_EQ(disappear.entity_id_list(0), b_role);
        }
    }

    tw.capture.Clear();
    tw.world->MoveEntity(b, JPH::Vec3(kNearX + 2.0f, 0.0f, kNearZ + 2.0f), EntityPropertyType::kMove);
    tw.world->Tick();

    const CapturedFrame* af = tw.capture.FirstFrame(a->GetId(), proto_id("cli_3d_aoi_appears_ntf"));
    EXPECT_TRUE(af != nullptr);
    if (af) {
        ::cli_3d_aoi_appears_ntf appear;
        EXPECT_TRUE(appear.ParseFromString(af->body));
        EXPECT_GE(appear.list_size(), 1);
        if (appear.list_size() >= 1) {
            EXPECT_EQ(appear.list(0).entity_id(), b_role);
        }
    }
}

GAME_TEST(AoiProtoTest, DirtySinglePlayerReceivesOwnSync) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    EXPECT_TRUE(a);
    tw.capture.Clear();
    tw.world->Tick();

    a->SetPropertyDirty(EntityPropertyType::kMove);
    tw.world->Tick();

    EXPECT_EQ(tw.capture.CountFrames(a->GetId(), proto_id("cli_3d_aoi_appears_ntf")), 1u);
}

GAME_TEST(AoiProtoTest, MultipleDirtyTypesMergeIntoOneSync) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    auto b = SpawnPlayer(tw, kNearX + 1.0f, kNearZ + 1.0f);
    EXPECT_TRUE(a && b);
    tw.capture.Clear();
    tw.world->Tick();

    a->SetPropertyDirty(EntityPropertyType::kMove);
    a->SetPropertyDirty(EntityPropertyType::kUseEmoji);
    a->SetPropertyDirty(EntityPropertyType::kMove);  // 重复标记应合并
    tw.world->Tick();

    EXPECT_EQ(tw.capture.CountFrames(b->GetId(), proto_id("cli_3d_aoi_appears_ntf")), 1u);
}

GAME_TEST(AoiProtoTest, DirtyNotBroadcastWhenNotInMap) {
    ProtoTestWorld tw;
    // 用 Spawn（不进图）创建
    EntitySpawn spawn;
    spawn.position = JPH::Vec3(kNearX, 0.0f, kNearZ);
    auto a = tw.world->Spawn(EntityType::kPlayer, spawn);
    EXPECT_TRUE(a);
    EXPECT_FALSE(a->IsInMap());

    tw.capture.Clear();
    // 未进图标脏：SetPropertyDirty 内部 world_ 为空时不会调 MarkPropertyDirty
    // （Spawn 出来的实体 world_ 已绑定，但未进 AOI 格，MarkPropertyDirty 会 EnsureCell
    //  创建格但无观察者，FlushDirty 无观察者可推）
    a->SetPropertyDirty(EntityPropertyType::kMove);
    tw.world->Tick();

    EXPECT_EQ(tw.capture.frames.size(), 0u);
}

GAME_TEST(AoiProtoTest, ThreePlayersMutualAppear) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    auto b = SpawnPlayer(tw, kNearX + 1.0f, kNearZ + 1.0f);
    auto c = SpawnPlayer(tw, kNearX + 2.0f, kNearZ + 2.0f);
    EXPECT_TRUE(a && b && c);
    tw.world->Tick();

    EXPECT_EQ(tw.capture.CountFrames(a->GetId(), proto_id("cli_3d_aoi_appears_ntf")), 2u);
    EXPECT_EQ(tw.capture.CountFrames(b->GetId(), proto_id("cli_3d_aoi_appears_ntf")), 2u);
    EXPECT_EQ(tw.capture.CountFrames(c->GetId(), proto_id("cli_3d_aoi_appears_ntf")), 2u);
}

GAME_TEST(AoiProtoTest, ReenterMapTriggersAppearAgain) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    auto b = SpawnPlayer(tw, kNearX + 1.0f, kNearZ + 1.0f);
    EXPECT_TRUE(a && b);
    tw.world->Tick();

    // B 离图
    tw.world->LeaveMap(b);
    tw.world->Tick();
    // B 重新进图：先清空捕获，再 SpawnOnMap（进图回调在 SpawnOnMap 中同步触发）
    tw.capture.Clear();
    EntitySpawn respawn;
    respawn.position = JPH::Vec3(kNearX + 1.5f, 0.0f, kNearZ + 1.5f);
    auto b2 = tw.world->SpawnOnMap(EntityType::kPlayer, respawn);
    EXPECT_TRUE(b2);
    tw.world->Tick();

    const CapturedFrame* f = tw.capture.FirstFrame(a->GetId(), proto_id("cli_3d_aoi_appears_ntf"));
    EXPECT_TRUE(f != nullptr);
}

GAME_TEST(AoiProtoTest, AppearBodyHasAllFields) {
    ProtoTestWorld tw;
    auto a = SpawnPlayer(tw, kNearX, kNearZ);
    auto b = SpawnPlayer(tw, kNearX + 1.0f, kNearZ + 1.0f);
    EXPECT_TRUE(a && b);
    tw.world->Tick();

    const CapturedFrame* f = tw.capture.FirstFrame(a->GetId(), proto_id("cli_3d_aoi_appears_ntf"));
    EXPECT_TRUE(f != nullptr);
    if (f) {
        ::cli_3d_aoi_appears_ntf appear;
        EXPECT_TRUE(appear.ParseFromString(f->body));
        EXPECT_TRUE(appear.list_size() > 0);
        const auto& e3d = appear.list(0);
        EXPECT_TRUE(e3d.entity_data_list_size() > 0);  // entity_data_list 非空
    }
}

GAME_TEST(AoiProtoTest, BornPosMoveAwayAndBackReappear) {
    constexpr float kBornX = 333.0f;
    constexpr float kBornY = 18.0f;
    constexpr float kBornZ = 415.45f;
    ProtoTestWorld tw;
    EntitySpawn spawn_a;
    spawn_a.position = JPH::Vec3(kBornX, kBornY, kBornZ);
    auto a = tw.world->SpawnOnMap(EntityType::kPlayer, spawn_a);
    EntitySpawn spawn_b;
    spawn_b.position = JPH::Vec3(kBornX, kBornY, kBornZ);
    auto b = tw.world->SpawnOnMap(EntityType::kPlayer, spawn_b);
    EXPECT_TRUE(a && b);
    const uint64_t b_role = b->GetComponent<RoleComponent>()->role_id_;
    tw.capture.Clear();

    tw.world->MoveEntity(b, JPH::Vec3(1000.0f, 20.0f, 1000.0f), EntityPropertyType::kMove);
    tw.world->Tick();
    const CapturedFrame* df = tw.capture.FirstFrame(a->GetId(), proto_id("cli_3d_aoi_disappears_ntf"));
    EXPECT_TRUE(df != nullptr);

    tw.capture.Clear();
    tw.world->MoveEntity(b, JPH::Vec3(kBornX, kBornY, kBornZ), EntityPropertyType::kMove);
    tw.world->Tick();
    const CapturedFrame* af = tw.capture.FirstFrame(a->GetId(), proto_id("cli_3d_aoi_appears_ntf"));
    EXPECT_TRUE(af != nullptr);
    if (af) {
        ::cli_3d_aoi_appears_ntf appear;
        EXPECT_TRUE(appear.ParseFromString(af->body));
        EXPECT_TRUE(appear.list_size() > 0);
        EXPECT_EQ(appear.list(0).entity_id(), b_role);
    }
}
