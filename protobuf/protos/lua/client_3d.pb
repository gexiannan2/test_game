
¼
client_3d.protoclient_common.proto"U
cli_3d_move_req%
move (2.entity_move_dataRmove
	move_flag (RmoveFlag"}
cli_3d_move_res
	entity_id (RentityId&
err_code (2.error_codeRerrCode%
move (2.entity_move_dataRmove"8
cli_3d_jump_req%
jump (2.entity_jump_dataRjump"}
cli_3d_jump_res
	entity_id (RentityId&
err_code (2.error_codeRerrCode%
jump (2.entity_jump_dataRjump"<
cli_3d_dodge_req(
dodge (2.entity_dodge_dataRdodge"
cli_3d_dodge_res
	entity_id (RentityId&
err_code (2.error_codeRerrCode(
dodge (2.entity_dodge_dataRdodge"8
cli_3d_aoi_appears_ntf
list (2
.entity_3dRlist"7
cli_3d_aoi_update_ntf
list (2
.entity_3dRlist"A
cli_3d_aoi_disappears_ntf$
entity_id_list (RentityIdList"é
cli_3d_aoi_attr_update_ntf
	entity_id (RentityIdO
attr_changes (2,.cli_3d_aoi_attr_update_ntf.AttrChangesEntryRattrChanges

state_flag (R	stateFlag>
AttrChangesEntry
key (Rkey
value (Rvalue:8"¥
cli_3d_aoi_animation_ntf
	entity_id (RentityId!
animation_id (RanimationId
speed (Rspeed
loop (Rloop
client_time (R
clientTime"á
cli_3d_enter_map_ntf
cfg_id (RcfgId
map_id (RmapId
	source_id (	RsourceId$
role_entity_id (RroleEntityId
pos (2.vec3Rpos
rot (2.quatRrot&
err_code (2.error_codeRerrCode"F
cli_3d_leave_map_req
map_id (RmapId
op_code (RopCode"U
cli_3d_leave_map_res
map_id (RmapId&
err_code (2.error_codeRerrCode"n
cli_3d_leave_map_ntf
map_id (RmapId
op_code (RopCode&
err_code (2.error_codeRerrCodebproto3