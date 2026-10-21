forza_add_host_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/app.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/game_files.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/input.cpp)
target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_link_libraries(main PRIVATE SDL3::SDL3 tomlplusplus::tomlplusplus android log)
