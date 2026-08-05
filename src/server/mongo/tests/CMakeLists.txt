add_executable(mongo_tests MongoCrudTest.cpp)
add_executable(mongo_async_dispatch_tests MongoAsyncDispatcherTest.cpp)
add_executable(mongo_player_benchmark MongoPlayerBenchmark.cpp)

set(mongo_test_targets
    mongo_tests
    mongo_async_dispatch_tests
    mongo_player_benchmark
)

foreach(target IN LISTS mongo_test_targets)
    target_link_libraries(${target} PRIVATE mongo)
endforeach()

add_test(NAME mongo.crud COMMAND mongo_tests)
set_tests_properties(mongo.crud PROPERTIES TIMEOUT 45 LABELS "integration;mongodb")

add_test(NAME mongo.async_dispatch COMMAND mongo_async_dispatch_tests)
set_tests_properties(mongo.async_dispatch PROPERTIES TIMEOUT 45 LABELS "integration;mongodb;async")
