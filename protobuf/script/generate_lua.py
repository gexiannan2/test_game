"""
author: 李中昌
date: 2024-08-07
description: 生成 protobuf 协议代码
"""

import os.path
import subprocess
import sys
import shutil
import re
import zlib

from config import *
from common import *

msg_name_pattern = r"^\s*message\s+(\w+)\s*"

msg_reg_file_head = r'''-- 消息协议注册
return {{
{1}}}
'''

msg_name_2_msg_id_head = r'''-- 协议名对应协议id
return {{
{1}}}
'''


# 调用protoc 生成代码
def generate_lua():
    language = "lua"
    print(f"开始生成 {language} 代码 ========================================")

    output_dir = lua_output_dir
    if output_dir is None:
        print(f"未配置{language}输出目录, 跳过生成")
        sys.exit(1)


    # 遍历 proto 文件目录下的所有 .proto 文件
    for proto_dir in proto_dirs:
        output_dir_abs = os.path.join(proto_dir, output_dir)

        print("协议生成目录:", output_dir_abs)
        # 清理目录
        shutil.rmtree(output_dir_abs, ignore_errors=True)
        os.mkdir(output_dir_abs)

        for root, _, files in os.walk(proto_dir):
            reg_msg_code = ""
            msg_name_2_msg_id_code = ""
            for file in files:
                if not file.endswith('.proto'):
                    continue

                # print(f"正在处理: {file}")

                # 调用 protoc 命令，指定 proto 路径
                file_abs = os.path.join(root, file)
                # 去掉扩展名
                file_name = os.path.splitext(file)[0]
                cmd = [protoc_path, f"-I={root}", "-o", f"{output_dir_abs}/{file_name}.pb", file]
                # print(f"generate_lua cmd = {cmd}")
                # print(f"执行命令: {" ".join(cmd)}")
                result = subprocess.run(cmd, capture_output=True, text=True)
                if result.returncode != 0:
                    print(f"{file} 生成失败: {result.stderr}")
                    sys.exit(1)
                else:
                    pass
                    print(f"{file} 生成成功")

                # 读取文件内容
                with open(file_abs, "r", encoding='utf-8') as f:
                    lines = f.readlines()

                with open(file_abs, "r", encoding='utf-8') as f:
                    for line in lines:
                        match = re.search(msg_name_pattern, line)
                        if not match:
                            continue

                        msg_name = match.group(1)
                        if msg_name.startswith(tuple(auto_reg_prefix)):
                            # 符合名称的消息名自动注册
                            msg_id = zlib.crc32(msg_name.encode("utf-8"))
                            reg_msg_code += f'    {msg_name} = {msg_id},\n'

                        if file.startswith("client_"):
                            msg_id = zlib.crc32(msg_name.encode("utf-8"))
                            msg_name_2_msg_id_code += f'    {msg_name} = {msg_id},\n'


            # 开始自动注册
            print("start auto reg lua msg")
            proto_parent_dir = get_penulitimate_dir(os.path.abspath(root))
            print(f"协议目录的上级目录: {proto_parent_dir}")
            auto_reg_msg_file_path = os.path.join(root, lua_msg_reg_file_name + "_" + proto_parent_dir + ".lua")
            # 如果已存在 删除它
            if os.path.exists(auto_reg_msg_file_path):
                os.remove(auto_reg_msg_file_path)

            # 重新创建文件并写入内容
            reg_content = msg_reg_file_head.format(proto_parent_dir, reg_msg_code)
            with open(auto_reg_msg_file_path, "w", encoding='utf-8') as f:
                f.write(reg_content)


            # 开始写入协议名对应协议id文件
            print(f"start write {name_to_msg_id_file_name}")
            msg_id_file_path = os.path.join(root, name_to_msg_id_file_name + "_" + proto_parent_dir + ".lua")
            # 如果已存在 删除它
            if os.path.exists(msg_id_file_path):
                os.remove(msg_id_file_path)

            msg_name_2_msg_id_code = msg_name_2_msg_id_head.format(proto_parent_dir, msg_name_2_msg_id_code)
            with open(msg_id_file_path, "w", encoding='utf-8') as f:
                f.write(msg_name_2_msg_id_code)

            break


    print(f"结束生成 {language} 代码 ========================================\n")


if __name__ == "__main__":
    current_dir = os.getcwd()
    print(f"当前目录: {current_dir}")

    # 获取命令行参数
    args = sys.argv
    if len(args) < 2:
        print("需要命令行传入 proj_dir 参数, 协议生成中断")
        sys.exit(1)

    proj_dir = args[1]

    # 获取所有 proto 文件所在的目录
    proto_dirs = get_proto_dirs(proj_dir)
    print("处理目录: ")
    for dir in proto_dirs:
        print(dir)

    generate_lua()