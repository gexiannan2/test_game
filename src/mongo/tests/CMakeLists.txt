add_executable(mongo_tests MongoCrudTest.cpp)
add_executable(mongo_async_dispatch_tests MongoAsyncDispatcherTest.cpp)
add_executable(mongo_player_benchmark MongoPlayerBenchmark.cpp)
# AccountIdGenerator 测试不需要 mongo 驱动和 proto，直接编源文件即可（零外部依赖）
add_executable(mongo_account_id_generator_tests
    AccountIdGeneratorTests.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../src/AccountIdGenerator.cpp"
)
target_include_directories(mongo_account_id_generator_tests PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../include"
)

set(mongo_test_targets
    mongo_tests
    mongo_async_dispatch_tests
    mongo_player_benchmark
)
# 只有需要 mongo 驱动的测试才链 mongo INTERFACE
foreach(target IN LISTS mongo_test_targets)
    target_link_libraries(${target} PRIVATE mongo)
endforeach()

add_test(NAME mongo.crud COMMAND mongo_tests)
set_tests_properties(mongo.crud PROPERTIES TIMEOUT 45 LABELS "integration;mongodb")

add_test(NAME mongo.async_dispatch COMMAND mongo_async_dispatch_tests)
set_tests_properties(mongo.async_dispatch PROPERTIES TIMEOUT 45 LABELS "integration;mongodb;async")

add_test(NAME mongo.account_id_generator COMMAND mongo_account_id_generator_tests)
set_tests_properties(mongo.account_id_generator PROPERTIES TIMEOUT 30 LABELS "unit;id_generator")
