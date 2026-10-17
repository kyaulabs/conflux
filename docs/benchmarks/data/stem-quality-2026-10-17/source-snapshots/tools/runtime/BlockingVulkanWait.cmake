# SPDX-License-Identifier: AGPL-3.0-or-later

option(CONFLUX_STEM_BLOCKING_WAIT
    "Wait for Vulkan fences without CPU polling" ON)
target_compile_definitions(conflux-stem-runtime PRIVATE
    CONFLUX_STEM_BLOCKING_WAIT=$<BOOL:${CONFLUX_STEM_BLOCKING_WAIT}>)

if(CONFLUX_STEM_BLOCKING_WAIT)
    get_target_property(vulkan_source_dir ggml-vulkan SOURCE_DIR)
    set(vulkan_source "${vulkan_source_dir}/ggml-vulkan.cpp")
    file(SHA256 "${vulkan_source}" vulkan_source_hash)
    if(NOT vulkan_source_hash STREQUAL
            "e58a915da3620150a33cf8ac5cc4f454206d5c51221860e5fc9db91d7b590d1d")
        message(FATAL_ERROR "Blocking fence wait requires the pinned audio.cpp v0.9.2 Vulkan backend")
    endif()

    file(READ "${vulkan_source}" vulkan_source_content)
    string(REPLACE "\r\n" "\n" vulkan_source_content "${vulkan_source_content}")
    set(polling_wait [=[    // Spin (w/pause) waiting for the graph to finish executing.
    vk::Result result;
    while ((result = ctx->device->device.getFenceStatus(ctx->fence)) != vk::Result::eSuccess) {
        if (result != vk::Result::eNotReady) {
            fprintf(stderr, "ggml_vulkan: error %s at %s:%d\n", to_string(result).c_str(), __FILE__, __LINE__);
            exit(1);
        }
        for (uint32_t i = 0; i < 100; ++i) {
            YIELD();
            YIELD();
            YIELD();
            YIELD();
            YIELD();
            YIELD();
            YIELD();
            YIELD();
            YIELD();
            YIELD();
        }
    }
]=])
    set(blocking_wait [=[    // M0 experiment: leave the CPU idle while the GPU finishes the graph.
    VK_CHECK(ctx->device->device.waitForFences({ ctx->fence }, true, UINT64_MAX), "graph_fence");
]=])
    string(FIND "${vulkan_source_content}" "${polling_wait}" polling_wait_position)
    if(polling_wait_position EQUAL -1)
        message(FATAL_ERROR "Pinned Vulkan fence wait implementation was not found")
    endif()
    string(REPLACE "${polling_wait}" "${blocking_wait}"
        vulkan_source_content "${vulkan_source_content}")

    set(blocking_source "${CMAKE_CURRENT_BINARY_DIR}/conflux-vulkan/ggml-vulkan.cpp")
    file(CONFIGURE OUTPUT "${blocking_source}" CONTENT "${vulkan_source_content}" @ONLY)
    get_target_property(vulkan_sources ggml-vulkan SOURCES)
    list(FIND vulkan_sources "ggml-vulkan.cpp" vulkan_source_index)
    if(vulkan_source_index EQUAL -1)
        message(FATAL_ERROR "Pinned ggml-vulkan target source was not found")
    endif()
    list(REMOVE_AT vulkan_sources ${vulkan_source_index})
    list(APPEND vulkan_sources "${blocking_source}")
    set_property(TARGET ggml-vulkan PROPERTY SOURCES "${vulkan_sources}")
    message(STATUS "Conflux Vulkan fence wait: blocking (upstream source unchanged)")
else()
    message(STATUS "Conflux Vulkan fence wait: upstream polling")
endif()
