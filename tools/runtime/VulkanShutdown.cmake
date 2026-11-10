# SPDX-License-Identifier: AGPL-3.0-or-later

# Install teardown into the selected backend after the other build overlays.
# The adapter calls it only after its Vulkan execution context has unwound.
get_target_property(shutdown_upstream_dir ggml-vulkan SOURCE_DIR)
set(shutdown_upstream "${shutdown_upstream_dir}/ggml-vulkan.cpp")
file(SHA256 "${shutdown_upstream}" shutdown_upstream_hash)
if(NOT shutdown_upstream_hash STREQUAL
        "e58a915da3620150a33cf8ac5cc4f454206d5c51221860e5fc9db91d7b590d1d")
    message(FATAL_ERROR "Vulkan shutdown requires the pinned audio.cpp v0.9.2 Vulkan backend")
endif()

if(CONFLUX_STEM_FLASH_F32)
    set(shutdown_selected "${flash_vulkan}")
elseif(CONFLUX_STEM_BLOCKING_WAIT)
    set(shutdown_selected "${blocking_source}")
else()
    set(shutdown_selected "ggml-vulkan.cpp")
endif()
get_target_property(shutdown_sources ggml-vulkan SOURCES)
list(FIND shutdown_sources "${shutdown_selected}" shutdown_index)
if(shutdown_index EQUAL -1)
    message(FATAL_ERROR "Selected ggml-vulkan implementation was not found")
endif()

if(IS_ABSOLUTE "${shutdown_selected}")
    file(READ "${shutdown_selected}" shutdown_content)
else()
    file(READ "${shutdown_upstream}" shutdown_content)
endif()
set(shutdown_instance "static vk_instance_t vk_instance;")
set(shutdown_destructor "    ~vk_device_struct() {")
foreach(shutdown_anchor IN ITEMS "${shutdown_instance}" "${shutdown_destructor}")
    string(FIND "${shutdown_content}" "${shutdown_anchor}" shutdown_first)
    string(FIND "${shutdown_content}" "${shutdown_anchor}" shutdown_last REVERSE)
    if(shutdown_first EQUAL -1 OR NOT shutdown_first EQUAL shutdown_last)
        message(FATAL_ERROR "Expected one Vulkan shutdown source anchor: ${shutdown_anchor}")
    endif()
endforeach()
string(REPLACE "${shutdown_instance}" "${shutdown_instance}\n\n#include \"VulkanShutdown.inc\""
    shutdown_content "${shutdown_content}")
string(REPLACE "${shutdown_destructor}"
    "${shutdown_destructor}\n        if (!device) {\n            return;\n        }"
    shutdown_content "${shutdown_content}")
set(shutdown_source "${CMAKE_CURRENT_BINARY_DIR}/conflux-vulkan-shutdown/ggml-vulkan.cpp")
file(CONFIGURE OUTPUT "${shutdown_source}" CONTENT "${shutdown_content}" @ONLY)
list(REMOVE_AT shutdown_sources ${shutdown_index})
list(APPEND shutdown_sources "${shutdown_source}")
set_property(TARGET ggml-vulkan PROPERTY SOURCES "${shutdown_sources}")
target_include_directories(ggml-vulkan PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
