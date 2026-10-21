forza_add_host_sources(${CMAKE_CURRENT_SOURCE_DIR}/output.cpp)
target_include_directories(main PRIVATE ${SDK}/runtime ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(main PRIVATE SDL3::SDL3)
