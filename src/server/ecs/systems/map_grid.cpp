// 逻辑格实体增删（hash 索引，O(1)）。

#include "ecs/systems/map_grid.h"

#include "ecs/entity/entity.h"

bool MapGrid::AddEntity(EntityPtr entity) {
    if (!entity) {
        return false;
    }
    // try_emplace：key 已存在则不插入，返回 false；O(1) 平均
    const auto [it, ok] = entities_.try_emplace(entity->GetId(),
                                                 std::move(entity));
    return ok;
}

void MapGrid::RemoveEntity(uint64_t id) {
    entities_.erase(id);
}
