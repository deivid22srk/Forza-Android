forza_add_host_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/game_info.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hooks.cpp)
target_include_directories(main PRIVATE
    ${SDK}/runtime
    ${FORZA_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR})
