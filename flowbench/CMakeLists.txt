project (flowbench)

fdb_find_sources(FLOWBENCH_SRCS)

find_package(benchmark REQUIRED)

add_flow_target(EXECUTABLE NAME flowbench SRCS ${FLOWBENCH_SRCS})
target_include_directories(flowbench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include")
 
if(FLOW_USE_ZSTD)
   target_include_directories(flowbench PRIVATE ${ZSTD_LIB_INCLUDE_DIR})
endif()
target_link_libraries(flowbench PRIVATE benchmark::benchmark pthread flow fdbclient)
