// JoltServer 实现 — OBJ 解析 + Jolt 物理初始化 + 静态碰撞体

#include "jolt_server.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

#include "zrpc/base/logger.h"

// ============================================================
// BroadPhase 层接口实现
// ============================================================

JoltServer::BPLayerInterface::BPLayerInterface()
{
    for (JPH::ObjectLayer i = 0; i < PhysicsLayerConfig::NUM_LAYERS; ++i)
        object_to_bp_[i] = PhysicsLayerConfig::BP_NON_MOVING;
    object_to_bp_[PhysicsLayerConfig::MOVING] = PhysicsLayerConfig::BP_MOVING;
}

JPH::BroadPhaseLayer JoltServer::BPLayerInterface::GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const
{
    JPH_ASSERT(inLayer < PhysicsLayerConfig::NUM_LAYERS);
    return object_to_bp_[inLayer];
}

bool JoltServer::ObjVsBPLayerFilter::ShouldCollide(
    JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const
{
    if (inLayer1 == PhysicsLayerConfig::NON_MOVING)
        return inLayer2 == PhysicsLayerConfig::BP_MOVING;
    if (inLayer1 == PhysicsLayerConfig::MOVING)
        return inLayer2 == PhysicsLayerConfig::BP_NON_MOVING
            || inLayer2 == PhysicsLayerConfig::BP_MOVING;
    return false;
}

bool JoltServer::ObjLayerPairFilter::ShouldCollide(
    JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const
{
    if (inObject1 == PhysicsLayerConfig::NON_MOVING)
        return inObject2 == PhysicsLayerConfig::MOVING;
    if (inObject1 == PhysicsLayerConfig::MOVING)
        return inObject2 == PhysicsLayerConfig::NON_MOVING
            || inObject2 == PhysicsLayerConfig::MOVING;
    return false;
}

// ============================================================
// OBJ 解析（简化版，支持 v/f 指令）
// ============================================================

ObjLoadResult JoltServer::ParseObjFile(const std::string& path,
                                       bool flip_x, bool flip_y, bool flip_z)
{
    ObjLoadResult result;
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file)
    {
        result.error = "cannot open: " + path;
        return result;
    }

    std::vector<JPH::Float3> vertices;
    vertices.reserve(8192);
    std::vector<JPH::Triangle> triangles;
    triangles.reserve(4096);

    JPH::Vec3 bmin = JPH::Vec3::sReplicate(std::numeric_limits<float>::max());
    JPH::Vec3 bmax = JPH::Vec3::sReplicate(-std::numeric_limits<float>::max());

    std::string line;
    size_t face_count = 0;
    size_t line_no = 0;

    while (std::getline(file, line))
    {
        ++line_no;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);

        std::istringstream ss(line);
        std::string directive;
        if (!(ss >> directive)) continue;

        if (directive == "v")
        {
            float x, y, z;
            if (!(ss >> x >> y >> z))
            {
                result.error = "line " + std::to_string(line_no) + ": bad vertex";
                return result;
            }
            vertices.push_back(JPH::Float3(x, y, z));
            bmin.SetX(std::min(bmin.GetX(), x));
            bmin.SetY(std::min(bmin.GetY(), y));
            bmin.SetZ(std::min(bmin.GetZ(), z));
            bmax.SetX(std::max(bmax.GetX(), x));
            bmax.SetY(std::max(bmax.GetY(), y));
            bmax.SetZ(std::max(bmax.GetZ(), z));
        }
        else if (directive == "f")
        {
            int indices[4096];
            int idx_count = 0;
            std::string token;
            while (ss >> token && idx_count < 4096)
            {
                size_t slash = token.find('/');
                std::string vstr = (slash != std::string::npos) ? token.substr(0, slash) : token;
                if (vstr.empty()) continue;
                int vi = std::atoi(vstr.c_str());
                if (vi < 0) vi = static_cast<int>(vertices.size()) + vi + 1;
                if (vi < 1 || vi > static_cast<int>(vertices.size()))
                {
                    result.error = "line " + std::to_string(line_no) + ": vertex index out of range";
                    return result;
                }
                indices[idx_count++] = vi - 1;
            }
            if (idx_count < 3) continue;

            for (int i = 1; i + 1 < idx_count; ++i)
            {
                triangles.push_back(JPH::Triangle(
                    vertices[indices[0]],
                    vertices[indices[i]],
                    vertices[indices[i + 1]]));
            }
            ++face_count;
        }
    }

    if (triangles.empty())
    {
        result.error = "no triangles found in " + path;
        return result;
    }

    // 奇数轴翻转时 swap v1/v2 修正三角形绕序（法线方向）
    if (flip_x || flip_y || flip_z)
    {
        const int flip_count = (flip_x ? 1 : 0) + (flip_y ? 1 : 0) + (flip_z ? 1 : 0);
        const bool swap_winding = (flip_count % 2 == 1);

        for (auto& tri : triangles)
        {
            JPH::Vec3 v1 = JPH::Vec3::sLoadFloat3Unsafe(tri.mV[0]);
            JPH::Vec3 v2 = JPH::Vec3::sLoadFloat3Unsafe(tri.mV[1]);
            JPH::Vec3 v3 = JPH::Vec3::sLoadFloat3Unsafe(tri.mV[2]);

            if (flip_x) { v1.SetX(-v1.GetX()); v2.SetX(-v2.GetX()); v3.SetX(-v3.GetX()); }
            if (flip_y) { v1.SetY(-v1.GetY()); v2.SetY(-v2.GetY()); v3.SetY(-v3.GetY()); }
            if (flip_z) { v1.SetZ(-v1.GetZ()); v2.SetZ(-v2.GetZ()); v3.SetZ(-v3.GetZ()); }

            if (swap_winding)
                tri = JPH::Triangle(v1, v3, v2);
            else
                tri = JPH::Triangle(v1, v2, v3);
        }

        // 翻转后 bounds min/max 对应轴交换
        float bmin_arr[3] = {bmin.GetX(), bmin.GetY(), bmin.GetZ()};
        float bmax_arr[3] = {bmax.GetX(), bmax.GetY(), bmax.GetZ()};
        if (flip_x) { std::swap(bmin_arr[0], bmax_arr[0]); bmin_arr[0] = -bmin_arr[0]; bmax_arr[0] = -bmax_arr[0]; }
        if (flip_y) { std::swap(bmin_arr[1], bmax_arr[1]); bmin_arr[1] = -bmin_arr[1]; bmax_arr[1] = -bmax_arr[1]; }
        if (flip_z) { std::swap(bmin_arr[2], bmax_arr[2]); bmin_arr[2] = -bmin_arr[2]; bmax_arr[2] = -bmax_arr[2]; }
        bmin = JPH::Vec3(bmin_arr[0], bmin_arr[1], bmin_arr[2]);
        bmax = JPH::Vec3(bmax_arr[0], bmax_arr[1], bmax_arr[2]);
    }

    // 找距包围盒中心最近的上朝三角形质心
    JPH::Vec3 bounds_center = 0.5f * (bmin + bmax);
    JPH::Vec3 spawn_point = bounds_center;
    bool has_spawn = false;
    bool has_up_facing = false;
    double best_dist_sq = std::numeric_limits<double>::max();

    for (const auto& tri : triangles)
    {
        JPH::Vec3 p0 = JPH::Vec3::sLoadFloat3Unsafe(tri.mV[0]);
        JPH::Vec3 p1 = JPH::Vec3::sLoadFloat3Unsafe(tri.mV[1]);
        JPH::Vec3 p2 = JPH::Vec3::sLoadFloat3Unsafe(tri.mV[2]);
        JPH::Vec3 normal = (p1 - p0).Cross(p2 - p0);
        float normal_len = normal.Length();
        if (normal.GetY() <= normal_len * 1.0e-4f)
            continue;
        has_up_facing = true;
        JPH::Vec3 centroid = (p0 + p1 + p2) / 3.0f;
        double dx = double(centroid.GetX() - bounds_center.GetX());
        double dz = double(centroid.GetZ() - bounds_center.GetZ());
        double dist_sq = dx * dx + dz * dz;
        if (dist_sq < best_dist_sq)
        {
            best_dist_sq = dist_sq;
            spawn_point = centroid;
            has_spawn = true;
        }
    }

    if (!has_up_facing)
        LOG_WARN << "OBJ warning: no upward-facing triangles in " << path;

    result.ok = true;
    result.vertices = std::move(vertices);
    result.triangles = std::move(triangles);
    result.bounds.min = bmin;
    result.bounds.max = bmax;
    result.bounds.center = bounds_center;
    result.bounds.size = bmax - bmin;
    result.bounds.spawn_point = spawn_point;
    result.bounds.has_spawn_point = has_spawn;
    result.face_count = face_count;
    return result;
}

// ============================================================
// JoltServer 生命周期
// ============================================================

JoltServer::JoltServer() = default;

JoltServer::~JoltServer()
{
    Shutdown();
}

bool JoltServer::Init(uint32_t max_bodies)
{
    if (initialized_) return true;

    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    temp_allocator_ = std::make_unique<JPH::TempAllocatorImpl>(32 * 1024 * 1024);
    job_system_ = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 0);

    physics_system_ = std::make_unique<JPH::PhysicsSystem>();
    physics_system_->Init(
        max_bodies,
        0,
        131072,
        40960,
        bp_layer_interface_,
        obj_vs_bp_filter_,
        obj_pair_filter_);

    physics_system_->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

    initialized_ = true;
    LOG_INFO << "JoltServer initialized: max_bodies=" << max_bodies;
    return true;
}

bool JoltServer::LoadMap(const std::string& obj_path)
{
    if (!initialized_)
    {
        LOG_ERROR << "JoltServer not initialized, cannot load map";
        return false;
    }

    // 坐标系开关（默认全关 = 左手系直接使用）
    const char* fx = std::getenv("GAME_FLIP_X");
    const char* fy = std::getenv("GAME_FLIP_Y");
    const char* fz = std::getenv("GAME_FLIP_Z");
    flip_x_ = (fx && fx[0] == '1');
    flip_y_ = (fy && fy[0] == '1');
    flip_z_ = (fz && fz[0] == '1');

    ObjLoadResult obj = ParseObjFile(obj_path, flip_x_, flip_y_, flip_z_);
    if (!obj.ok)
    {
        LOG_ERROR << "OBJ parse failed: " << obj.error;
        return false;
    }

    LOG_INFO << "OBJ loaded: " << obj_path
             << " vertices=" << obj.vertices.size()
             << " faces=" << obj.face_count
             << " triangles=" << obj.triangles.size()
             << " flip=(" << flip_x_ << "," << flip_y_ << "," << flip_z_ << ")";
    LOG_INFO << "  bounds_min=(" << obj.bounds.min.GetX() << ","
             << obj.bounds.min.GetY() << "," << obj.bounds.min.GetZ() << ")";
    LOG_INFO << "  bounds_max=(" << obj.bounds.max.GetX() << ","
             << obj.bounds.max.GetY() << "," << obj.bounds.max.GetZ() << ")";
    LOG_INFO << "  center=(" << obj.bounds.center.GetX() << ","
             << obj.bounds.center.GetY() << "," << obj.bounds.center.GetZ() << ")";
    LOG_INFO << "  size=(" << obj.bounds.size.GetX() << ","
             << obj.bounds.size.GetY() << "," << obj.bounds.size.GetZ() << ")";
    if (obj.bounds.has_spawn_point)
        LOG_INFO << "  spawn_point=(" << obj.bounds.spawn_point.GetX() << ","
                 << obj.bounds.spawn_point.GetY() << "," << obj.bounds.spawn_point.GetZ() << ")";
    else
        LOG_WARN << "  spawn_point not found, fallback to center";

    // ---- 局部化到包围盒中心（提高浮点精度）----
    JPH::Vec3 body_position = JPH::Vec3::sZero();
    if (recenter_ && !obj.bounds.center.IsNearZero())
    {
        body_position = obj.bounds.center;
        JPH::Vec3 offset = -body_position;
        for (auto& tri : obj.triangles)
        {
            JPH::Vec3 v1 = JPH::Vec3::sLoadFloat3Unsafe(tri.mV[0]) + offset;
            JPH::Vec3 v2 = JPH::Vec3::sLoadFloat3Unsafe(tri.mV[1]) + offset;
            JPH::Vec3 v3 = JPH::Vec3::sLoadFloat3Unsafe(tri.mV[2]) + offset;
            tri = JPH::Triangle(v1, v2, v3);
        }
        LOG_INFO << "  recenter=true, body_position=("
                 << body_position.GetX() << "," << body_position.GetY() << ","
                 << body_position.GetZ() << ")";
    }
    else
    {
        LOG_INFO << "  recenter=false (triangles use original coordinates)";
    }

    if (!CreateStaticBody(obj.triangles, body_position))
    {
        LOG_ERROR << "CreateStaticBody failed";
        return false;
    }

    bounds_ = obj.bounds;
    map_loaded_ = true;

    LOG_INFO << "JoltServer map loaded, body_id=" << map_body_id_.GetIndexAndSequenceNumber()
             << " body_pos=(" << body_position.GetX() << ","
             << body_position.GetY() << "," << body_position.GetZ() << ")";
    return true;
}

void JoltServer::Shutdown()
{
    if (!initialized_) return;

    if (map_loaded_ && !map_body_id_.IsInvalid())
    {
        auto& bi = physics_system_->GetBodyInterface();
        bi.RemoveBody(map_body_id_);
        bi.DestroyBody(map_body_id_);
        map_body_id_ = JPH::BodyID();
        map_loaded_ = false;
    }

    physics_system_.reset();
    job_system_.reset();
    temp_allocator_.reset();

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;

    initialized_ = false;
}

// ============================================================
// 静态碰撞体创建
// ============================================================

bool JoltServer::CreateStaticBody(const std::vector<JPH::Triangle>& triangles,
                                   JPH::Vec3Arg body_position)
{
    if (triangles.empty())
    {
        LOG_ERROR << "CreateStaticBody: empty triangles";
        return false;
    }

    JPH::MeshShapeSettings mesh_settings(
        JPH::Array<JPH::Triangle>(triangles.data(), triangles.data() + triangles.size()));

    // 检查 Jolt 是否删除了退化/重复三角形
    if (mesh_settings.mIndexedTriangles.size() != triangles.size())
    {
        LOG_WARN << "Jolt removed " << (triangles.size() - mesh_settings.mIndexedTriangles.size())
                 << " degenerate/duplicate triangles (input=" << triangles.size()
                 << " kept=" << mesh_settings.mIndexedTriangles.size() << ")";
    }

    auto shape_result = mesh_settings.Create();
    if (shape_result.HasError())
    {
        LOG_ERROR << "MeshShape failed: " << shape_result.GetError().c_str();
        return false;
    }

    auto& bi = physics_system_->GetBodyInterface();
    map_body_id_ = bi.CreateAndAddBody(
        JPH::BodyCreationSettings(
            shape_result.Get(),
            body_position,
            JPH::Quat::sIdentity(),
            JPH::EMotionType::Static,
            PhysicsLayerConfig::NON_MOVING),
        JPH::EActivation::DontActivate);

    LOG_INFO << "static mesh body created, triangles=" << triangles.size()
             << " body_id=" << map_body_id_.GetIndexAndSequenceNumber();
    return true;
}

// ============================================================
// 物理步进
// ============================================================

void JoltServer::Update(float dt, int collision_steps)
{
    if (!initialized_ || !physics_system_) return;
    physics_system_->Update(dt, collision_steps, temp_allocator_.get(), job_system_.get());
}

JPH::BodyInterface& JoltServer::GetBodyInterface()
{
    return physics_system_->GetBodyInterface();
}

// ============================================================
// 通用 Body 管理
// ============================================================

JPH::BodyID JoltServer::CreateKinematicBody(JPH::Shape* shape,
                                              JPH::Vec3Arg center, JPH::QuatArg rot)
{
    if (!initialized_ || !physics_system_) return JPH::BodyID();

    JPH::BodyCreationSettings settings(
        shape, center, rot,
        JPH::EMotionType::Kinematic,
        PhysicsLayerConfig::MOVING);

    auto& bi = GetBodyInterface();
    JPH::BodyID body_id = bi.CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (body_id.IsInvalid())
    {
        LOG_WARN << "CreateKinematicBody FAILED";
    }
    return body_id;
}

void JoltServer::RemoveBody(const JPH::BodyID& body_id)
{
    if (body_id.IsInvalid()) return;
    auto& bi = GetBodyInterface();
    bi.RemoveBody(body_id);
    bi.DestroyBody(body_id);
}

void JoltServer::SetBodyPosition(const JPH::BodyID& body_id, JPH::Vec3Arg center)
{
    if (body_id.IsInvalid()) return;
    auto& bi = GetBodyInterface();
    bi.SetPosition(body_id, center, JPH::EActivation::Activate);
}

void JoltServer::MoveKinematicBody(const JPH::BodyID& body_id,
                                     JPH::Vec3Arg target_center, JPH::QuatArg rot, float dt)
{
    if (body_id.IsInvalid()) return;
    if (dt < 1e-6f) dt = 1e-6f;
    auto& bi = GetBodyInterface();
    bi.MoveKinematic(body_id, target_center, rot, dt);
}

void JoltServer::SetBodyVelocity(const JPH::BodyID& body_id, JPH::Vec3Arg vel)
{
    if (body_id.IsInvalid()) return;
    auto& bi = GetBodyInterface();
    bi.SetLinearVelocity(body_id, vel);
}

JPH::Vec3 JoltServer::GetBodyPosition(const JPH::BodyID& body_id) const
{
    if (body_id.IsInvalid()) return JPH::Vec3::sZero();
    const auto& bi = const_cast<JoltServer*>(this)->GetBodyInterface();
    return bi.GetPosition(body_id);
}
