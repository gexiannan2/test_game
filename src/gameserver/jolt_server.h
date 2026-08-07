// JoltServer — 物理世界 + 地图场景管理
//
// 职责：
//   1. 解析 Wavefront OBJ 文件 → 提取三角形网格 + AABB 包围盒
//   2. 初始化 JoltPhysics（PhysicsSystem / TempAllocator / JobSystem）
//   3. 从三角形创建静态碰撞体（MeshShape）
//   4. 提供地图坐标范围供 AOI/移动验证
//
// 坐标系：
//   OBJ 源坐标 (Unity: Y-up) → Jolt 坐标 (Y-up)
//   默认翻转 X 轴（Unity 左手系 → Jolt 右手系）

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace JPH
{
class PhysicsSystem;
class TempAllocatorImpl;
class JobSystemThreadPool;
class BodyInterface;
}  // namespace JPH

// ============================================================
// 碰撞层配置（移植自 JoltServer PhysicsLayerSystem）
// ============================================================

class PhysicsLayerConfig
{
public:
    static constexpr JPH::ObjectLayer NON_MOVING = 4;
    static constexpr JPH::ObjectLayer MOVING     = 5;
    static constexpr JPH::ObjectLayer NUM_LAYERS  = 8;

    static constexpr JPH::BroadPhaseLayer BP_NON_MOVING{0};
    static constexpr JPH::BroadPhaseLayer BP_MOVING{1};
    static constexpr JPH::uint BP_NUM_LAYERS = 2;
};

// ============================================================
// 地图 AABB 包围盒
// ============================================================

struct MapBounds
{
    JPH::Vec3 min = JPH::Vec3::sZero();
    JPH::Vec3 max = JPH::Vec3::sZero();
    JPH::Vec3 center = JPH::Vec3::sZero();
    JPH::Vec3 size = JPH::Vec3::sZero();
    JPH::Vec3 spawn_point = JPH::Vec3::sZero();  // 出生点（最近的上朝三角形质心）
    bool has_spawn_point = false;

    bool Contains(JPH::Vec3Arg pos) const
    {
        return pos.GetX() >= min.GetX() && pos.GetX() <= max.GetX()
            && pos.GetY() >= min.GetY() && pos.GetY() <= max.GetY()
            && pos.GetZ() >= min.GetZ() && pos.GetZ() <= max.GetZ();
    }
};

// ============================================================
// OBJ 解析结果
// ============================================================

struct ObjLoadResult
{
    bool ok = false;
    std::string error;
    std::vector<JPH::Float3> vertices;
    std::vector<JPH::Triangle> triangles;
    MapBounds bounds;
    size_t face_count = 0;
};

// ============================================================
// JoltServer
// ============================================================

class JoltServer
{
public:
    JoltServer();
    ~JoltServer();

    // ---- 生命周期 ----

    // 初始化 Jolt 物理系统
    bool Init(uint32_t max_bodies = 500000);

    // OBJ 三角形是否居中到包围盒中心（默认 false，保持原始坐标）
    void SetRecenter(bool v) { recenter_ = v; }
    bool IsRecenter() const  { return recenter_; }

    // 加载 OBJ 地图文件，创建静态碰撞体
    bool LoadMap(const std::string& obj_path);

    // 销毁
    void Shutdown();

    // ---- 查询 ----

    bool IsInitialized() const { return initialized_; }
    bool IsMapLoaded() const { return map_loaded_; }

    const MapBounds& GetBounds() const { return bounds_; }
    JPH::PhysicsSystem* GetPhysicsSystem() { return physics_system_.get(); }
    JPH::BodyInterface& GetBodyInterface();

    // 坐标范围验证
    bool IsInBounds(JPH::Vec3Arg pos) const { return bounds_.Contains(pos); }

    // 物理步进
    void Update(float dt, int collision_steps = 1);

    // ---- 通用 Body 管理（不感知 entity_id/胶囊/脚底坐标）----

    // 创建运动学体，center 为 Body 中心世界坐标
    JPH::BodyID CreateKinematicBody(JPH::Shape* shape,
                                     JPH::Vec3Arg center, JPH::QuatArg rot);

    // 删除体
    void RemoveBody(const JPH::BodyID& body_id);

    // 瞬移体位置（center 为 Body 中心坐标）
    void SetBodyPosition(const JPH::BodyID& body_id, JPH::Vec3Arg center);

    // 平滑移动体（Jolt 内部算速度，Update 时积分到目标）
    void MoveKinematicBody(const JPH::BodyID& body_id,
                            JPH::Vec3Arg target_center, JPH::QuatArg rot, float dt);

    // 获取体位置（返回中心坐标）
    JPH::Vec3 GetBodyPosition(const JPH::BodyID& body_id) const;

private:
    // BroadPhase 层接口
    class BPLayerInterface final : public JPH::BroadPhaseLayerInterface
    {
    public:
        BPLayerInterface();
        JPH::uint GetNumBroadPhaseLayers() const override { return PhysicsLayerConfig::BP_NUM_LAYERS; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override;
    private:
        JPH::BroadPhaseLayer object_to_bp_[PhysicsLayerConfig::NUM_LAYERS];
    };

    class ObjVsBPLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override;
    };

    class ObjLayerPairFilter final : public JPH::ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override;
    };

    // OBJ 解析
    // flip_x/flip_y/flip_z: 独立翻转各轴（奇数翻转时自动修正三角形绕序）
    static ObjLoadResult ParseObjFile(const std::string& path,
                                       bool flip_x = false,
                                       bool flip_y = false,
                                       bool flip_z = false);

    // 从三角形创建静态 MeshShape Body
    bool CreateStaticBody(const std::vector<JPH::Triangle>& triangles,
                          JPH::Vec3Arg body_position);

    // ---- 成员 ----

    bool initialized_ = false;
    bool map_loaded_ = false;
    bool recenter_   = false;  // OBJ 三角形是否居中，默认 false
    bool flip_x_ = false;
    bool flip_y_ = false;
    bool flip_z_ = false;

    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator_;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system_;
    std::unique_ptr<JPH::PhysicsSystem> physics_system_;

    BPLayerInterface bp_layer_interface_;
    ObjVsBPLayerFilter obj_vs_bp_filter_;
    ObjLayerPairFilter obj_pair_filter_;

    JPH::BodyID map_body_id_;
    MapBounds bounds_;
};
