# The graphics backend. Milestone 1 is a null backend; the Vulkan translation
# arrives in native/gfx/vulkan/ and replaces gpu::CreateBackend here.
forza_add_host_sources(${CMAKE_CURRENT_SOURCE_DIR}/backend.cpp)
target_include_directories(main PRIVATE ${SDK}/runtime ${CMAKE_CURRENT_SOURCE_DIR})
