
ê
client_login.protoclient_common.proto"-
cli_handshake_req
version (	Rversion"M
cli_handshake_res&
err_code (2.error_codeRerrCode
msg (	Rmsg"[
cli_user_login_req
uid (	Ruid
token (	Rtoken

channel_id (R	channelId"x
cli_user_login_res&
err_code (2.error_codeRerrCode

session_id (R	sessionId
	gate_addr (	RgateAddr"ç
cli_role_info
role_id (RroleId
name (	Rname
sex (Rsex
job (Rjob
re_level (RreLevel
create_time (R
createTime
deny_ip (RdenyIp!
deny_account (RdenyAccount
deny_uid	 (RdenyUid
deny_msg
 (	RdenyMsg
	deny_time (RdenyTime
new_char (RnewChar
	lock_char (RlockChar
last (Rlast
looks (Rlooks"2
cli_role_list_req

session_id (R	sessionId"a
cli_role_create_req

session_id (R	sessionId+
	role_info (2.cli_role_infoRroleInfo"M
cli_role_delete_req

session_id (R	sessionId
role_id (RroleId"ø
cli_role_list_res&
err_code (2.error_codeRerrCode
op_code (RopCode+
	role_list (2.cli_role_infoRroleList
forbid_edit (R
forbidEdit
	line_list (RlineList"Ä
cli_role_login_req

session_id (R	sessionId
role_id (RroleId
op_code (RopCode
line_idx (RlineIdx"n
cli_role_login_res&
err_code (2.error_codeRerrCode
role_id (RroleId
op_code (RopCode"
cli_enter_game_req"<
cli_enter_game_res&
err_code (2.error_codeRerrCode"
cli_global_config_ntf"
cli_heart_beat_req"]
cli_heart_beat_res&
err_code (2.error_codeRerrCode
server_time (R
serverTime"2
cli_reconnect_req

session_id (R	sessionId"Z
cli_reconnect_res

session_id (R	sessionId&
err_code (2.error_codeRerrCode"'
cli_random_name_req
sex (Rsex"^
cli_random_name_res
random_name (	R
randomName&
err_code (2.error_codeRerrCode"b
cli_kickoff_player_ntf
role_id (RroleId
code_id (RcodeId
reason (	Rreasonbproto3