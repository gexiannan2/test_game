#include "utils/proto_fill.h"

#include "ecs/components/role_component.h"
#include "ecs/components/transform_component.h"

namespace proto_fill {

void FillPlayerData(::entity_player_data* out, uint64_t id,
                    const TransformComponent* tfm, const RoleComponent* role) {
  if (!out) return;
  auto* base = out->mutable_base();
  base->set_id(role ? role->role_id_ : id);
  base->set_type(::ENTITY_PLAYER);
  FillEntityBase(base, tfm);

  if (!role) return;
  auto* bd = out->mutable_base_data();
  bd->set_lv(role->level_);
  bd->set_job(role->job_);
  bd->set_sex(role->sex_);
  if (!role->name_.empty()) bd->set_name(role->name_);
}

}  // namespace proto_fill
