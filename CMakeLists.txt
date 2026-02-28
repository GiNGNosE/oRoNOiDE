# Copyright 2026 oRoNoiDE
cmake_minimum_required(VERSION 3.24)
project(oRoNoiDE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# find dependencies
find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED)

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
)
target_include_directories(oronoide_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
oronoide_enable_warnings(oronoide_core)

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
target_link_libraries(oronoide_core PUBLIC SDL3::SDL3)

add_executable(stone_rendering samples/stone_rendering/main.cpp)
target_link_libraries(stone_rendering PRIVATE oronoide_render)
