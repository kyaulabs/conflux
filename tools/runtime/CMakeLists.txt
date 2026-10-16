cmake_minimum_required(VERSION 3.25)
project(ConfluxStemRuntime LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(AUDIOCPP_SOURCE_DIR "" CACHE PATH "audio.cpp v0.9.2 source checkout")
if(NOT EXISTS "${AUDIOCPP_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set AUDIOCPP_SOURCE_DIR to the verified audio.cpp v0.9.2 source")
endif()
file(SHA256 "${AUDIOCPP_SOURCE_DIR}/CMakeLists.txt" runtime_cmake_hash)
file(SHA256 "${AUDIOCPP_SOURCE_DIR}/src/models/demucs/pipeline.cpp" runtime_pipeline_hash)
if(NOT runtime_cmake_hash STREQUAL "5970918d431172b3ae12e0990a003fb87298c44b8e1849ec4e1c68f5301e9d34"
        OR NOT runtime_pipeline_hash STREQUAL "0da057a313fcf868c9e4be2fb27bd42d802a3f9ddd53cf98f0b55d9f98a52673")
    message(FATAL_ERROR "audio.cpp source does not match the pinned v0.9.2 build interface")
endif()

set(AUDIOCPP_MODEL_SET custom CACHE STRING "" FORCE)
set(AUDIOCPP_MODELS demucs CACHE STRING "" FORCE)
set(AUDIOCPP_DEPLOYMENT_BUILD ON CACHE BOOL "" FORCE)
set(AUDIOCPP_BUILD_C_API OFF CACHE BOOL "" FORCE)
set(AUDIOCPP_BUILD_NATIVE_MODEL_MANAGER OFF CACHE BOOL "" FORCE)
set(ENGINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENGINE_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
find_path(SPIRV_HEADERS_INCLUDE_DIR
    NAMES spirv/unified1/spirv.hpp spirv-headers/spirv.hpp spirv.hpp REQUIRED)
add_subdirectory("${AUDIOCPP_SOURCE_DIR}" audiocpp EXCLUDE_FROM_ALL)
target_include_directories(ggml-vulkan PRIVATE "${SPIRV_HEADERS_INCLUDE_DIR}")

add_executable(conflux-stem-runtime htdemucs_segment.cpp)
target_link_libraries(conflux-stem-runtime PRIVATE engine_runtime)
target_compile_options(conflux-stem-runtime PRIVATE -Wall -Wextra -Wpedantic)
