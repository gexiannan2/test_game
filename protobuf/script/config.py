import platform

# 生成代码目录
output_dir_dict = {
    "cpp": "./c++",
}

lua_output_dir = "./lua"

# protoc 路径
protoc_path = "protoc.exe"


# proto目录 (包含所有的proto文件的目录)
# proto_dir = "./pb"


# 如果是linux平台
if platform.system() == "Linux":
    protoc_path = "protoc"


# proto文件扫描目录
# scan_paths = ["protos", "src", "test", "proto", "release"]
scan_paths = ["protos"]

# 自动注册消息的前缀 (只有 svc_ 前缀的服务端协议才写入 Msg_Svr)
auto_reg_prefix = ["svc_"]

# lua 消息注册文件名
lua_msg_reg_file_name = "Msg_Svr"

# lua 消息名对映协议名称文件
name_to_msg_id_file_name = "Msg_Cli"
