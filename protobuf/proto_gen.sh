#!/bin/sh

# 生成代码文件
python3 ./script/generate.py

# 暂停1s
# timeout /t 1

# 修改cpp代码文件
python3 ./script/modify.py
