import os
import sys

from config import *


def get_proto_dirs(proj_dir):
    # 递归扫描找出所有包含.proto文件的目录
    proto_dirs = set()
    if proj_dir.lower().endswith("protos"):
        proto_dirs.add(proj_dir)


    for path in scan_paths:
        for root, dirs, files in os.walk(os.path.join(proj_dir, path)):
            for file in files:
                if file.endswith(".proto"):
                    proto_dirs.add(root)
                    break

    return proto_dirs


def get_penulitimate_dir(path_str):
    normalized = os.path.normpath(path_str)
    parts = normalized.split(os.sep)
    parts = [p for p in parts if p]
    if len(parts) < 2:
        print(f"协议目录层级错误: {path_str}")
        sys.exit(1)

    return parts[-2]