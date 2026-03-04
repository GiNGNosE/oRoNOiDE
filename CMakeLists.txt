# Copyright 2026 oRoNoiDE
cmake_minimum_required(VERSION 3.24)
project(oRoNoiDE LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# find dependencies
find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED)
find_package(glm QUIET)
find_program(GLSLC glslc REQUIRED)

if(NOT TARGET glm::glm)
    include(FetchContent)
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glm)
endif()

#helper for strict warnings
function(oronoide_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
endfunction()

# core
add_library(oronoide_core STATIC
    engine/core/log.cpp
    engine/core/app.cpp
    engine/core/camera.cpp
)
target_include_directories(oronoide_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
oronoide_enable_warnings(oronoide_core)

# voxel
add_library(oronoide_voxel STATIC
    engine/voxel/voxel_world.cpp
    engine/voxel/edit_classifier.cpp
    engine/voxel/side_effect_fences.cpp
    engine/voxel/transaction.cpp
    engine/voxel/dual_contouring.cpp
    engine/voxel/async_publication.cpp
    engine/voxel/hard_edit_gate.cpp
    engine/voxel/quality_gates.cpp
    engine/voxel/ci_protocol.cpp
    engine/voxel/ci_lanes.cpp
    engine/voxel/snapshot_registry.cpp
    engine/voxel/rollback_state_store.cpp
    engine/voxel/worker_runtime.cpp
    engine/voxel/runtime.cpp
)
target_include_directories(oronoide_voxel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
oronoide_enable_warnings(oronoide_voxel)

# material
add_library(oronoide_material STATIC
    engine/material/registry.cpp
)
target_include_directories(oronoide_material PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
oronoide_enable_warnings(oronoide_material)

# vulkan rhi
add_library(oronoide_rhi_vulkan STATIC
    engine/rhi/vulkan/context.cpp
    engine/rhi/vulkan/swapchain.cpp
)
target_include_directories(oronoide_rhi_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
oronoide_enable_warnings(oronoide_rhi_vulkan)

# render
add_library(oronoide_render STATIC
    engine/render/renderer.cpp
)
target_include_directories(oronoide_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
oronoide_enable_warnings(oronoide_render)
target_link_libraries(oronoide_rhi_vulkan PUBLIC oronoide_core Vulkan::Vulkan)
target_link_libraries(oronoide_render PUBLIC oronoide_rhi_vulkan)
target_link_libraries(oronoide_render PUBLIC glm::glm)
target_link_libraries(oronoide_core PUBLIC SDL3::SDL3 oronoide_voxel oronoide_material)
target_link_libraries(oronoide_core PUBLIC glm::glm)

set(ORO_SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine/render/shaders)
set(ORO_SHADER_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(ORO_SHADER_SOURCES
    ${ORO_SHADER_SOURCE_DIR}/voxel_mesh.vert
    ${ORO_SHADER_SOURCE_DIR}/voxel_mesh.frag
    ${ORO_SHADER_SOURCE_DIR}/shadow_depth.vert
    ${ORO_SHADER_SOURCE_DIR}/shadow_depth.frag
)

set(ORO_SHADER_BINARIES)
foreach(shader_source IN LISTS ORO_SHADER_SOURCES)
    get_filename_component(shader_name ${shader_source} NAME)
    set(shader_binary ${ORO_SHADER_BINARY_DIR}/${shader_name}.spv)
    add_custom_command(
        OUTPUT ${shader_binary}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ORO_SHADER_BINARY_DIR}
        COMMAND ${GLSLC} ${shader_source} -o ${shader_binary}
        DEPENDS ${shader_source}
        VERBATIM
    )
    list(APPEND ORO_SHADER_BINARIES ${shader_binary})
endforeach()

add_custom_target(oronoide_shaders ALL DEPENDS ${ORO_SHADER_BINARIES})

add_executable(stone_rendering samples/stone_rendering/main.cpp)
target_link_libraries(stone_rendering PRIVATE oronoide_render)
add_dependencies(stone_rendering oronoide_shaders)
add_custom_command(TARGET stone_rendering POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:stone_rendering>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORO_SHADER_BINARIES} $<TARGET_FILE_DIR:stone_rendering>/shaders
)

add_executable(oronoide_voxel_runtime_tests
    tests/voxel/runtime_phase_v4_tests.cpp
)
target_link_libraries(oronoide_voxel_runtime_tests PRIVATE oronoide_voxel)
oronoide_enable_warnings(oronoide_voxel_runtime_tests)
add_test(NAME voxel_runtime_phase_v4 COMMAND oronoide_voxel_runtime_tests)
