#pragma once

// 单个地图逻辑格：通行状态 + 本格实体集合（按 entity id 索引，O(1) 增删）。

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "common/aoi_def.h"

class MapGrid {
 public:
    MapGrid() : key_{} {}
    explicit MapGrid(GridKey key, uint32_t status)
        : key_(key), status_(status) {}

    void SetStatus(uint32_t status) { status_ = status; }
    uint32_t GetStatus() const { return status_; }
    bool CheckStatus(uint32_t check_type) const {
        return (status_ & check_type) != 0;
    }

    bool AddEntity(EntityPtr entity);
    void RemoveEntity(uint64_t id);
    const std::unordered_map<uint64_t, EntityPtr>& GetEntities() const {
        return entities_;
    }
    const GridKey& GetKey() const { return key_; }

 private:
    GridKey key_{};
    uint32_t status_ = 0;
    std::unordered_map<uint64_t, EntityPtr> entities_;
};
