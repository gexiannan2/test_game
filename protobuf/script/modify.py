"""
author: 李中昌
date: 2024-08-15
description: 修改 protobuf 协议代码
"""

import shutil
import re
import sys
import zlib

from config import *
from common import *

# 类名称匹配正则
class_name_pattern = r'@@protoc_insertion_point\(class_definition:([^)]*)\)'
# 消息类需要增加的代码
msg_class_add_code = r'''{0}
 public:
  const std::string msg_name() override
  {{
    return "{1}";    
  }}
  uint32_t msg_id() override
  {{
    return {2};
  }}
  static uint32_t get_msg_id()
  {{
    return {2};
  }}
  
  const std::string to_json() override
  {{
    std::string proto_json;
    google::protobuf::util::MessageToJsonString(*this, &proto_json);
    
    std::string name_and_id = R"({{"msg_name":"{1}","msg_id":{2})";
    if (proto_json.size() > 2)
    {{
      name_and_id += ",";
    }}
    proto_json.replace(0, 1, name_and_id);
    
    return proto_json;
  }}
  
   void from_json(const std::string& proto_json) override
   {{
     std::regex pattern(R"(^\{{"msg_name":"[^"]+","msg_id":[0-9]+,)");
     std::smatch match;
     std::string msg_content = proto_json;
     if (std::regex_search(proto_json, match, pattern))
     {{
         msg_content.replace(0, match[0].length(), "{{");
     }}

     google::protobuf::util::JsonStringToMessage(msg_content, this);
   }}
  
'''

# 修改protobuf消息定义 & 增加注册消息的代码
def modify_cpp(proto_dir):
    language = "cpp"
    output_dir = output_dir_dict[language]
    if output_dir is None:
        print(f"未配置 {language}协议 输出目录, 无法修改消息定义和注册消息!")
        sys.exit(1)

    output_dir = os.path.join(proto_dir, output_dir)
    if not os.path.exists(output_dir):
        print(f"未找到 {language}协议 输出目录: {output_dir}, 跳过修改代码")
        sys.exit(1)


    # 遍历 output_dir, 查找头文件
    for root, dirs, files in os.walk(output_dir):
        for file in files:
            if not file.endswith("pb.h"):
                continue

            # 注册消息的代码
            reg_msg_code = ""

            # 注册消息需要导入头文件
            import_headers_code = f'#include "{file}"\n'
            header_file_path = os.path.join(root, file)
            # 读取文件
            with open(header_file_path, "r", encoding='utf-8') as f:
                lines = f.readlines()


            # 存储已经注册过的res消息
            already_reg_res = set()
            pending_reg_res = set()

            # 修改消息文件
            # 整理消息注册需要修改的代码
            with open(header_file_path, "w", encoding='utf-8') as f:
                for line in lines:
                    if "public ::PROTOBUF_NAMESPACE_ID::Message" in line or "public ::PROTOBUF_NAMESPACE_ID::internal::ZeroFieldsBase" in line:
                        # 找到类名称
                        match = re.search(class_name_pattern, line)
                        if not match:
                            raise Exception(f"can't find class_name from {header_file_path}:{line}")

                        msg_name = match.group(1)
                        msg_id = zlib.crc32(msg_name.encode("utf-8"))

                        # 修改消息类代码
                        line = msg_class_add_code.format(line, msg_name, msg_id)

                        # 根据消息名前缀判断是否需要自动注册消息
                        if msg_name.startswith(tuple(auto_reg_prefix)):
                            if msg_name.endswith("_req"):
                                if not msg_name.startswith("cli_"):
                                    msg_name_origin = msg_name[:-len("_req")]
                                    reg_msg_code += f'        msg_reg_request({msg_name}, {msg_name_origin}_res);\n'

                                    already_reg_res.add(f'{msg_name_origin}_res')
                                else:
                                    reg_msg_code += f'        msg_reg_normal({msg_name});\n'
                            elif msg_name.endswith("_res"):
                                pending_reg_res.add(msg_name)
                            else:
                                reg_msg_code += f'        msg_reg_normal({msg_name});\n'

                    elif "#include <google/protobuf/unknown_field_set.h>" in line:
                        line = line + '#include <google/protobuf/util/json_util.h>\n#include <regex>\n'

                    f.write(line)

                for res_name in pending_reg_res:
                    if res_name in already_reg_res:
                        continue
                    reg_msg_code += f'        msg_reg_normal({res_name});\n'

            print(f"修改文件: {header_file_path} 完成")

            # 拷贝文件
            file_without_ext = file.split('.')[0]
            target_file_name = f"{file_without_ext}.auto.cpp"
            shutil.copyfile("./script/msg_reg.cpp", os.path.join(output_dir, target_file_name))

            print(f"开始编写消息注册代码 ========================================\n")
            msg_reg_cpp = os.path.join(output_dir, target_file_name)
            # proto_dir_crc32 = zlib.crc32(proto_dir.encode("utf-8"))

            with open(msg_reg_cpp, "r", encoding='utf-8') as f:
                msg_reg_cpp_all_lines = f.readlines()

            with open(msg_reg_cpp, "w", encoding='utf-8') as f:
                for line in msg_reg_cpp_all_lines:
                    if '#include "e996_msg.h"' in line:
                        line = f"{line}\n{import_headers_code}"
                    elif 'namespace e996' in line:
                        line = f"namespace {file_without_ext}\n"
                    # elif 'int reg_msg()' in line:
                    #     line = f"    int reg_msg_{proto_dir_crc32}()\n"
                    elif r"// 开始注册消息" in line:
                        line = f"{line}{reg_msg_code}\n        return 0;\n"
                    # elif 'auto _ = reg_msg();' in line:
                    #     line = f"    auto _ = reg_msg_{proto_dir_crc32}();\n"
                    f.write(line)

            print(f"结束编写消息注册代码 ========================================\n")



if __name__ == "__main__":
    print(f"开始修改 c++ 协议代码 ========================================")
    current_dir = os.getcwd()
    print(f"当前目录: {current_dir}")

    # 获取命令行参数
    args = sys.argv
    if len(args) < 2:
        print("需要命令行传入 proj_dir 参数, 协议修改中断")
        sys.exit(1)

    proj_dir = args[1]
    print(f"处理目录: {proj_dir}")

    # 获取所有 proto 文件所在的目录
    proto_dirs = get_proto_dirs(proj_dir)
    print("proto_dirs: ")
    for dir in proto_dirs:
        print(dir)
        modify_cpp(dir)

    print(f"结束修改 c++协议代码 ========================================\n")
