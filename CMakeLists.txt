cmake_minimum_required(VERSION 3.5.0)
set(TARGET_NAME mysql_scanner)
project(${TARGET_NAME})
option(MYSQL_GUARDED_CRUD "Build explicit guarded CRUD runtime" OFF)

set(libmariadb_DIR ${CMAKE_CURRENT_LIST_DIR}/vcpkg_ports/libmariadb)
find_package(libmariadb REQUIRED)
set(EXTENSION_NAME ${TARGET_NAME}_extension)

set(MYSQL_INCLUDE_DIR
    ${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include/mysql)
include_directories(${MYSQL_INCLUDE_DIR})
include_directories(database-connector/src/include)

add_subdirectory(src)

set(PARAMETERS "-no-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS} ${ALL_OBJECT_FILES})

# Loadable binary
target_include_directories(${TARGET_NAME}_loadable_extension
                           PRIVATE include ${MYSQL_INCLUDE_DIR})
target_link_libraries(${TARGET_NAME}_loadable_extension ${MYSQL_LIBRARIES})

option(MYSQL_BUILD_RESOURCE_TESTS "构建无数据库缓存生命周期测试" OFF)
if(MYSQL_BUILD_RESOURCE_TESTS)
  add_executable(mysql_plan_cache_resource_test test/plan_cache_resource_test.cpp
                 src/storage/federation/plan_cache.cpp)
  target_include_directories(mysql_plan_cache_resource_test PRIVATE src/include)
  target_link_libraries(mysql_plan_cache_resource_test duckdb)
  add_executable(mysql_statistics_cache_resource_test
                 test/statistics_cache_resource_test.cpp ${ALL_OBJECT_FILES})
  target_include_directories(mysql_statistics_cache_resource_test
                             PRIVATE src/include ${MYSQL_INCLUDE_DIR})
  target_link_libraries(mysql_statistics_cache_resource_test
                        duckdb duckdb_yyjson ${MYSQL_LIBRARIES})
  enable_testing()
  add_test(NAME mysql_plan_cache_resources COMMAND mysql_plan_cache_resource_test)
  add_test(NAME mysql_statistics_cache_resources COMMAND mysql_statistics_cache_resource_test)
endif()
