# Dear ImGui overlay and options menu: milestone 3, when the Vulkan backend can
# draw it. The file exists so the module boundary stays in place.
forza_add_host_sources(${CMAKE_CURRENT_SOURCE_DIR}/overlay.cpp)
target_include_directories(main PRIVATE ${SDK}/runtime)
