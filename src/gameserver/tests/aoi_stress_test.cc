// AOI 大规模压测 + 生命周期异常（2000 人进离、churn、标脏、边界不崩溃）。

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "client_3d.pb.h"
#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"
#include "ecs/entity/entity.h"
#include "ecs/entity/player_entity.h"
#include "protocol/pack_flags.h"
#include "common/aoi_def.h"
#include "ecs/systems/aoi_system.h"
#include "ecs/systems/map_system.h"
#include "ecs/systems/world_system.h"
#include "test_harness.h"

namespace {

// 简单确定性随机数（可重现）
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
    uint32_t Next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<uint32_t>((s * 0x2545F4914F6CDD1DULL) >> 32);
    }
    float RangeF(float lo, float hi) {
        return lo + (hi - lo) * (Next() % 10000) / 10000.0f;
    }
    int Range(int lo, int hi) { return lo + Next() % (hi - lo + 1); }
};

// kGridSize=4, kAoiRadius=0：视野为单格 10×10×10 立方
constexpr uint32_t kGrid = 8;

// 构造世界 + PlayerEntity 工厂（带 RoleComponent）
struct StressWorld {
    std::shared_ptr<WorldSystem> world;

    StressWorld() {
        world = WorldSystem::Create(SceneRegionType::kMap);
        world->Init();
        world->SetEntityFactory([](WorldSystem* w, uint64_t id, EntityType type,
                                    const EntitySpawn& spawn) -> EntityPtr {
            auto e = std::make_shared<PlayerEntity>(w, id, type, spawn);
            e->AddComponent<RoleComponent>();
            auto* role = e->GetComponent<RoleComponent>();
            role->role_id_ = id * 100;
            role->name_ = "p" + std::to_string(id);
            role->level_ = 1;
            role->job_ = 1;
            role->sex_ = 1;
            return e;
        });
    }
};

EntityPtr SpawnAt(StressWorld& sw, float x, float z) {
    EntitySpawn spawn;
    spawn.position = JPH::Vec3(x, 0.0f, z);
    return sw.world->SpawnOnMap(EntityType::kPlayer, spawn);
}

}  // namespace

GAME_TEST_SUITE(AoiStressTest);

GAME_TEST(AoiStressTest, TwoThousandEnterLeave) {
    StressWorld sw;
    constexpr int N = 2000;
    Rng rng(42);

    std::vector<EntityPtr> players;
    players.reserve(N);
    for (int i = 0; i < N; ++i) {
        // 在 200x200 范围内分散生成
        float x = rng.RangeF(0.0f, 200.0f * kGrid);
        float z = rng.RangeF(0.0f, 200.0f * kGrid);
        auto e = SpawnAt(sw, x, z);
        EXPECT_TRUE(e != nullptr);
        if (e) players.push_back(e);
    }
    sw.world->Tick();
    EXPECT_EQ(sw.world->GetEntityCount(), static_cast<size_t>(players.size()));

    // 全部离图
    for (auto& e : players) {
        sw.world->LeaveMap(e);
    }
    players.clear();
    sw.world->Tick();
    EXPECT_EQ(sw.world->GetEntityCount(), 0u);
}

GAME_TEST(AoiStressTest, TwoThousandChurnEnterLeave) {
    StressWorld sw;
    constexpr int N = 2000;
    constexpr int Rounds = 5;
    Rng rng(123);

    for (int round = 0; round < Rounds; ++round) {
        std::vector<EntityPtr> players;
        players.reserve(N);
        for (int i = 0; i < N; ++i) {
            float x = rng.RangeF(0.0f, 200.0f * kGrid);
            float z = rng.RangeF(0.0f, 200.0f * kGrid);
            auto e = SpawnAt(sw, x, z);
            if (e) players.push_back(e);
        }
        sw.world->Tick();
        EXPECT_EQ(sw.world->GetEntityCount(), static_cast<size_t>(players.size()));

        // 随机离线一半
        for (size_t i = 0; i < players.size(); ++i) {
            if (rng.Next() % 2 == 0) {
                sw.world->LeaveMap(players[i]);
                players[i].reset();
            }
        }
        sw.world->Tick();

        // 剩余离线
        for (auto& e : players) {
            if (e) sw.world->LeaveMap(e);
        }
        sw.world->Tick();
        EXPECT_EQ(sw.world->GetEntityCount(), 0u);
    }
}

GAME_TEST(AoiStressTest, TwoThousandMoveAndDirtySync) {
    StressWorld sw;
    constexpr int N = 2000;
    constexpr int MoveSteps = 5;
    Rng rng(777);

    std::vector<EntityPtr> players;
    players.reserve(N);
    for (int i = 0; i < N; ++i) {
        float x = rng.RangeF(0.0f, 200.0f * kGrid);
        float z = rng.RangeF(0.0f, 200.0f * kGrid);
        auto e = SpawnAt(sw, x, z);
        if (e) players.push_back(e);
    }
    sw.world->Tick();

    // 多步随机移动 + 标脏
    for (int step = 0; step < MoveSteps; ++step) {
        for (auto& e : players) {
            if (!e) continue;
            float nx = rng.RangeF(0.0f, 200.0f * kGrid);
            float nz = rng.RangeF(0.0f, 200.0f * kGrid);
            sw.world->MoveEntity(e, JPH::Vec3(nx, 0.0f, nz), EntityPropertyType::kMove);
            // 随机标脏
            if (rng.Next() % 3 == 0) {
                e->SetPropertyDirty(EntityPropertyType::kMove);
            }
        }
        sw.world->Tick();  // FlushDirty
    }

    // 清理
    for (auto& e : players) {
        if (e) sw.world->LeaveMap(e);
    }
    sw.world->Tick();
    EXPECT_EQ(sw.world->GetEntityCount(), 0u);
}

GAME_TEST(AoiStressTest, TwoThousandSameCell) {
    StressWorld sw;
    constexpr int N = 2000;

    std::vector<EntityPtr> players;
    players.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto e = SpawnAt(sw, 1.0f, 1.0f);  // 全部同格
        if (e) players.push_back(e);
    }
    sw.world->Tick();
    EXPECT_EQ(sw.world->GetEntityCount(), static_cast<size_t>(players.size()));

    // 全部标脏 + Tick
    for (auto& e : players) {
        if (e) e->SetPropertyDirty(EntityPropertyType::kMove);
    }
    sw.world->Tick();

    // 逆序离图（测试迭代器稳定性）
    for (auto it = players.rbegin(); it != players.rend(); ++it) {
        if (*it) sw.world->LeaveMap(*it);
    }
    players.clear();
    sw.world->Tick();
    EXPECT_EQ(sw.world->GetEntityCount(), 0u);
}

GAME_TEST(AoiStressTest, DoubleEnterMapNoCrash) {
    StressWorld sw;
    auto e = SpawnAt(sw, 1.0f, 1.0f);
    EXPECT_TRUE(e);
    if (!e) return;
    EXPECT_TRUE(e->IsInMap());

    // 再次 EnterMap（重复进图）
    sw.world->EnterMap(e);
    sw.world->Tick();
    EXPECT_TRUE(e->IsInMap());

    sw.world->LeaveMap(e);
    sw.world->Tick();
    EXPECT_FALSE(e->IsInMap());
}

GAME_TEST(AoiStressTest, DoubleLeaveMapNoCrash) {
    StressWorld sw;
    auto e = SpawnAt(sw, 1.0f, 1.0f);
    EXPECT_TRUE(e);
    if (!e) return;

    sw.world->LeaveMap(e);
    EXPECT_FALSE(e->IsInMap());

    // 再次 LeaveMap（重复离图）
    sw.world->LeaveMap(e);
    sw.world->Tick();
    EXPECT_FALSE(e->IsInMap());
}

GAME_TEST(AoiStressTest, LeaveThenReenterNoCrash) {
    StressWorld sw;
    auto e = SpawnAt(sw, 1.0f, 1.0f);
    EXPECT_TRUE(e);
    if (!e) return;

    sw.world->LeaveMap(e);
    sw.world->Tick();
    EXPECT_FALSE(e->IsInMap());

    // 重新进图（需要重新注册，因为 LeaveMap 会 UnregisterEntity）
    sw.world->RegisterEntity(e);
    sw.world->EnterMap(e);
    sw.world->Tick();
    EXPECT_TRUE(e->IsInMap());

    sw.world->LeaveMap(e);
    sw.world->Tick();
}

GAME_TEST(AoiStressTest, MoveEntityBeforeEnterMapNoCrash) {
    StressWorld sw;
    // 用 Spawn（不进图）创建
    EntitySpawn spawn;
    spawn.position = JPH::Vec3(1.0f, 0.0f, 1.0f);
    auto e = sw.world->Spawn(EntityType::kPlayer, spawn);
    EXPECT_TRUE(e);
    if (!e) return;
    EXPECT_FALSE(e->IsInMap());

    // 未进图移动——MoveEntity 内部检查 IsInMap，应安全跳过
    sw.world->MoveEntity(e, JPH::Vec3(100.0f, 0.0f, 100.0f), EntityPropertyType::kMove);
    sw.world->Tick();
    EXPECT_FALSE(e->IsInMap());  // 仍未进图
}

GAME_TEST(AoiStressTest, DirtyBeforeEnterMapNoCrash) {
    StressWorld sw;
    EntitySpawn spawn;
    spawn.position = JPH::Vec3(1.0f, 0.0f, 1.0f);
    auto e = sw.world->Spawn(EntityType::kPlayer, spawn);
    EXPECT_TRUE(e);
    if (!e) return;

    // 未进图标脏——world_ 已绑定但未在 AOI 格中
    e->SetPropertyDirty(EntityPropertyType::kMove);
    sw.world->Tick();  // FlushDirty 应无观察者、不崩溃
    EXPECT_FALSE(e->IsInMap());
}

GAME_TEST(AoiStressTest, NullEntityNoCrash) {
    StressWorld sw;
    EntityPtr null_e;
    // 空指针进图/离图/移动——应安全跳过
    sw.world->EnterMap(null_e);
    sw.world->LeaveMap(null_e);
    sw.world->MoveEntity(null_e, JPH::Vec3(1.0f, 0.0f, 1.0f), EntityPropertyType::kMove);
    sw.world->Tick();
    EXPECT_EQ(sw.world->GetEntityCount(), 0u);
}

GAME_TEST(AoiStressTest, IteratorInvalidationDuringTick) {
    StressWorld sw;
    std::vector<EntityPtr> players;
    constexpr int N = 500;
    Rng rng(999);

    for (int i = 0; i < N; ++i) {
        auto e = SpawnAt(sw, rng.RangeF(0, 100 * kGrid), rng.RangeF(0, 100 * kGrid));
        if (e) players.push_back(e);
    }
    sw.world->Tick();

    // 标脏一半实体
    for (size_t i = 0; i < players.size(); i += 2) {
        if (players[i]) players[i]->SetPropertyDirty(EntityPropertyType::kMove);
    }

    // Tick 前随机移除部分实体（模拟在刷新过程中实体离线）
    for (size_t i = 0; i < players.size(); ++i) {
        if (players[i] && rng.Next() % 5 == 0) {
            sw.world->LeaveMap(players[i]);
            players[i].reset();
        }
    }
    // Tick 刷新脏属性——被移除的实体不应导致迭代器失效崩溃
    sw.world->Tick();

    // 清理
    for (auto& e : players) {
        if (e) sw.world->LeaveMap(e);
    }
    sw.world->Tick();
    EXPECT_EQ(sw.world->GetEntityCount(), 0u);
}

GAME_TEST(AoiStressTest, MassCrossSwapPositions) {
    StressWorld sw;
    constexpr int N = 500;
    std::vector<EntityPtr> players;
    players.reserve(N);

    // 分两组：A 组在左，B 组在右
    for (int i = 0; i < N; ++i) {
        float x = (i < N / 2) ? 1.0f : 500.0f;
        float z = static_cast<float>(i * kGrid);
        auto e = SpawnAt(sw, x, z);
        if (e) players.push_back(e);
    }
    sw.world->Tick();

    // 交换两组位置（大量跨格移动）
    for (int i = 0; i < N / 2; ++i) {
        if (players[i] && players[i + N / 2]) {
            sw.world->MoveEntity(players[i], JPH::Vec3(500.0f, 0.0f, static_cast<float>(i * kGrid)), EntityPropertyType::kMove);
            sw.world->MoveEntity(players[i + N / 2], JPH::Vec3(1.0f, 0.0f, static_cast<float>(i * kGrid)));
        }
    }
    sw.world->Tick();

    // 清理
    for (auto& e : players) {
        if (e) sw.world->LeaveMap(e);
    }
    sw.world->Tick();
    EXPECT_EQ(sw.world->GetEntityCount(), 0u);
}

GAME_TEST(AoiStressTest, TwoThousandMixedStress) {
    StressWorld sw;
    constexpr int N = 2000;
    Rng rng(2024);

    std::vector<EntityPtr> players;
    players.reserve(N);

    // 一半同格，一半分散
    for (int i = 0; i < N; ++i) {
        float x = (i < N / 2) ? 1.0f : rng.RangeF(0.0f, 300.0f * kGrid);
        float z = (i < N / 2) ? 1.0f : rng.RangeF(0.0f, 300.0f * kGrid);
        auto e = SpawnAt(sw, x, z);
        if (e) players.push_back(e);
    }
    sw.world->Tick();

    // 随机移动 + 标脏 + 部分离图
    for (int step = 0; step < 3; ++step) {
        for (size_t i = 0; i < players.size(); ++i) {
            if (!players[i]) continue;
            int action = rng.Next() % 4;
            switch (action) {
                case 0:  // 移动
                    sw.world->MoveEntity(players[i],
                        JPH::Vec3(rng.RangeF(0, 300 * kGrid), 0, rng.RangeF(0, 300 * kGrid)),
                        EntityPropertyType::kMove);
                    break;
                case 1:  // 标脏
                    players[i]->SetPropertyDirty(EntityPropertyType::kMove);
                    break;
                case 2:  // 离图
                    sw.world->LeaveMap(players[i]);
                    players[i].reset();
                    break;
                case 3:  // 无操作
                    break;
            }
        }
        sw.world->Tick();
    }

    // 清理剩余
    for (auto& e : players) {
        if (e) sw.world->LeaveMap(e);
    }
    sw.world->Tick();
    EXPECT_EQ(sw.world->GetEntityCount(), 0u);
}

GAME_TEST(AoiStressTest, PlayerCapsuleDefaults) {
    StressWorld sw;
    auto e = SpawnAt(sw, 1.0f, 1.0f);
    EXPECT_TRUE(e);
    if (!e) return;
    EXPECT_NEAR(e->GetHeight(), 1.5f, 0.001f);
    EXPECT_NEAR(e->GetRadius(), 0.3f, 0.001f);

    // 修改后验证
    e->SetHeight(2.0f);
    e->SetRadius(0.5f);
    EXPECT_NEAR(e->GetHeight(), 2.0f, 0.001f);
    EXPECT_NEAR(e->GetRadius(), 0.5f, 0.001f);
}

GAME_TEST(AoiStressTest, MoveRotIndependentFromRot) {
    StressWorld sw;
    auto e = SpawnAt(sw, 1.0f, 1.0f);
    EXPECT_TRUE(e);
    if (!e) return;

    // 设置不同的 move_rot 和 rot
    JPH::Quat rot = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 0.5f);
    JPH::Quat move_rot = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 1.0f);
    e->SetDirection(rot, EntityPropertyType::kMove);
    e->SetMoveRot(move_rot, EntityPropertyType::kMove);

    EXPECT_NEAR(e->GetMoveRot().GetY(), move_rot.GetY(), 0.001f);
    EXPECT_NEAR(e->GetMoveRot().GetW(), move_rot.GetW(), 0.001f);
    // rot 和 move_rot 应不同（Y 轴旋转：y=sin(θ/2), w=cos(θ/2)）
    EXPECT_TRUE(e->GetMoveRot().GetW() != e->GetDirection().GetW());
}
