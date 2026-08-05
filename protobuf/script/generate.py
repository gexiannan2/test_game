"""
author: 李中昌
date: 2024-08-07
description: 生成 protobuf 协议代码
"""

import os.path
import subprocess
import sys
import shutil

from config import *
from common import *


# 删除文件夹下所有内容
# def clean_dir(dir):
#     if not os.path.exists(dir):
#         return
#
#     for file in os.listdir(dir):
#         file_path = os.path.join(dir, file)
#         if os.path.isfile(file_path):
#             os.remove(file_path)
#         elif os.path.isdir(file_path):
#             shutil.rmtree(file_path)



# 调用protoc 生成代码
def generate_code(language="cpp"):
    print(f"开始生成 {language} 代码 ========================================")

    output_dir = output_dir_dict[language]
    if output_dir is None:
        print(f"未配置{language}输出目录, 跳过生成")
        return


    # 遍历 proto 文件目录下的所有 .proto 文件
    for proto_dir in proto_dirs:
        for root, _, files in os.walk(proto_dir):
            output_dir_abs = os.path.join(root, output_dir)
            print("协议生成目录:", output_dir_abs)

            # 清理目录
            shutil.rmtree(output_dir_abs, ignore_errors=True)
            os.mkdir(output_dir_abs)

            for file in files:
                if not file.endswith('.proto'):
                    continue

                # print(f"正在处理: {file}")

                # 调用 protoc 命令，指定 proto 路径
                cmd = [protoc_path, f"-I={root}", f"--{language}_out={output_dir_abs}", file]
                # print(f"执行命令: {" ".join(cmd)}")
                result = subprocess.run(cmd, capture_output=True, text=True)
                if result.returncode != 0:
                    print(f"{file} 生成失败: {result.stderr}")
                    sys.exit(1)
                else:
                    print(f"{file} 生成成功")

            break

    print(f"结束生成 {language} 代码 ========================================\n")


def generate_all():
    for language in output_dir_dict.keys():
        generate_code(language)


if __name__ == "__main__":
    current_dir = os.getcwd()
    print(f"当前目录: {current_dir}")

    # 获取命令行参数
    args = sys.argv
    if len(args) < 2:
        print("需要命令行传入 proj_dir 参数, 协议生成中断")
        sys.exit(1)

    proj_dir = args[1]
    print(f"处理目录: {proj_dir}")

    # 获取所有 proto 文件所在的目录
    proto_dirs = get_proto_dirs(proj_dir)
    print("proto_dirs: ")
    for dir in proto_dirs:
        print(dir)

    generate_all()