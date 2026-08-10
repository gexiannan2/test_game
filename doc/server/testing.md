<!-- 拆自 CODE_FLOWS；权威目录 doc/server/ -->

## 19. 测试体系

入口：`tests/test_main.cc`；宏：`GAME_TEST`（`test_harness.h`）

| 文件 | 用途 |
|------|------|
| aoi_system_test | AOI/Map 进离移、回调 |
| aoi_broadcast/parity | 广播条数与 Oracle 对拍 |
| aoi_proto_test | 真实序列化 + 桥接字段 |
| aoi_stress / aoi_mass_10k | 压力与万人对拍（环境变量门控） |
| map_system_test / math_test | 无界格、坐标换算 |
| handler_logic / production* | System 链、索引、出生点 |
| regression / extreme / anomaly | 历史回归与边界 |
| entity_id / recursive_scan | ID 与索引扫描 |
| test_aoi_oracle / test_*_invariants | Oracle 与不变量基建 |

构建目标示例：`svc_game_3d_test`（见根 `CMakeLists.txt`）。

---
