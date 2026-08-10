# Mongo 玩家异步持久化模块

目录：`src/mongo`  
供游戏服以源码方式编译的 MongoDB 玩家数据异步落地模块。

保留内容：

- 玩家快照异步投递：`AsyncMongoDispatcher`、`PlayerMongoStorage`
- MongoDB 客户端配置与连接池：`MongoClient`、`MongoConfig`
- 头文件在 `include/*.h`（`#include "PlayerMongoStorage.h"`）
- 驱动源码在仓库根：`third_party/mongo-c-driver`（2.3.3）、`third_party/mongo-cxx-driver`（r4.4.1）
- CRUD、异步派发和 QPS 基准测试

在根 `CMakeLists.txt` 中接入（已默认开启）：

```cmake
option(GAME_ENABLE_MONGO "接入玩家 Mongo 异步落地" ON)
if(GAME_ENABLE_MONGO)
  add_subdirectory("${SRC_DIR}/server/mongo")
  target_link_libraries(svc_game_3d_server PRIVATE mongo)
endif()
```

`mongo` 是 INTERFACE 源码目标，模块业务 `.cpp` 会直接编译进最终可执行文件。
不使用 vcpkg、`find_package(mongocxx)`、预编译 MongoDB DLL/SO。

独立运行本模块测试：

```bash
cmake -S src/mongo -B build/mongo -DMONGO_BUILD_TESTS=ON
cmake --build build/mongo --config Release
ctest --test-dir build/mongo --output-on-failure
```
