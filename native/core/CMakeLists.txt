# log.cpp replaces sdk/runtime/core/log.cpp (same logx API, sink policy tuned
# for logcat); the exclusion lives in native/runtime/CMakeLists.txt.
forza_add_host_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/log.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logcat.cpp)
target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
