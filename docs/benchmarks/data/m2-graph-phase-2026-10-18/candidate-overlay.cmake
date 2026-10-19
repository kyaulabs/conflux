# SPDX-License-Identifier: AGPL-3.0-or-later
# Isolated candidate only: include AFTER the unchanged GraphPhaseTiming.cmake.
# Reorder each finalized Vulkan graph once, before allocation; no upstream edits.
if(NOT DEFINED phase_pipeline OR NOT EXISTS "${phase_pipeline}"
        OR NOT TARGET engine_model_demucs)
    message(FATAL_ERROR "Graph order candidate requires the completed graph phase timing overlay")
endif()

set(graph_order_upstream_pipeline "${AUDIOCPP_SOURCE_DIR}/src/models/demucs/pipeline.cpp")
set(graph_order_backend_impl "${AUDIOCPP_SOURCE_DIR}/external/ggml/src/ggml-backend-impl.h")
set(graph_order_vulkan_header "${AUDIOCPP_SOURCE_DIR}/external/ggml/include/ggml-vulkan.h")
set(graph_order_vulkan_source "${AUDIOCPP_SOURCE_DIR}/external/ggml/src/ggml-vulkan/ggml-vulkan.cpp")
file(SHA256 "${graph_order_upstream_pipeline}" graph_order_pipeline_hash)
file(SHA256 "${graph_order_backend_impl}" graph_order_backend_impl_hash)
file(SHA256 "${graph_order_vulkan_header}" graph_order_vulkan_header_hash)
file(SHA256 "${graph_order_vulkan_source}" graph_order_vulkan_source_hash)
if(NOT graph_order_pipeline_hash STREQUAL
        "0da057a313fcf868c9e4be2fb27bd42d802a3f9ddd53cf98f0b55d9f98a52673"
        OR NOT graph_order_backend_impl_hash STREQUAL
        "43be4654d5af5f768485ab4f7c46560efcbbbaa810a43f73c2e945e7728073ef"
        OR NOT graph_order_vulkan_header_hash STREQUAL
        "0b6f625021b047907d24b60b3ca441e8a97d44235f0d32abeb2665112c35418e"
        OR NOT graph_order_vulkan_source_hash STREQUAL
        "e58a915da3620150a33cf8ac5cc4f454206d5c51221860e5fc9db91d7b590d1d")
    message(FATAL_ERROR "Graph order candidate requires the pinned pipeline and Vulkan backend interface/implementation")
endif()

# Reject any other transformation, a repeated include or the wrong source target.
file(SHA256 "${phase_pipeline}" graph_order_phase_hash)
if(NOT graph_order_phase_hash STREQUAL
        "f06814211f00eab41487caacefe1c2492dfe1d2a0ddd6da1f90bdbcc566fc8f9")
    message(FATAL_ERROR "Graph order candidate requires the exact generated phase timing pipeline")
endif()
get_target_property(graph_order_model_sources engine_model_demucs SOURCES)
list(FIND graph_order_model_sources "${phase_pipeline}" graph_order_source_index)
if(graph_order_source_index EQUAL -1)
    message(FATAL_ERROR "Graph order candidate generated pipeline is not a model target source")
endif()

file(READ "${phase_pipeline}" graph_order_content)
set(graph_order_include_anchor "#include <ggml-backend.h>")
set(graph_order_include_replacement [=[#include <ggml-backend.h>
#include <ggml-backend-impl.h>
#include <ggml-vulkan.h>
#include <array>
#include <cstdio>
#include <functional>]=])
set(graph_order_finalize_anchor [=[    void finalize_graph(size_t max_nodes) {
        graph_ = ggml_new_graph_custom(ctx_.get(), max_nodes, false);
        for (auto * output : outputs_) {
            ggml_set_output(output);
            ggml_build_forward_expand(graph_, output);
        }
        gallocr_ = ggml_gallocr_new(ggml_backend_get_default_buffer_type(backend_));
        if (gallocr_ == nullptr || !ggml_gallocr_alloc_graph(gallocr_, graph_)) {
            throw std::runtime_error("failed to allocate HTDemucs graph buffer");
        }
    }]=])
set(graph_order_finalize_replacement [=[    void finalize_graph(size_t max_nodes) {
        graph_ = ggml_new_graph_custom(ctx_.get(), max_nodes, false);
        for (auto * output : outputs_) {
            ggml_set_output(output);
            ggml_build_forward_expand(graph_, output);
        }
        const bool reorder_vulkan = ggml_backend_is_vk(backend_);
        size_t moved_positions = 0;
        if (reorder_vulkan) {
            if (backend_->iface.graph_optimize == nullptr) {
                throw std::runtime_error("Vulkan graph order callback is unavailable");
            }
            const int original_count = ggml_graph_n_nodes(graph_);
            auto ** original_array = ggml_graph_nodes(graph_);
            std::vector<ggml_tensor *> original_nodes(original_array, original_array + original_count);
            std::array<size_t, GGML_OP_COUNT> original_ops{};
            for (auto * node : original_nodes) {
                ++original_ops[node->op];
            }
            backend_->iface.graph_optimize(backend_, graph_);
            if (ggml_graph_n_nodes(graph_) != original_count) {
                throw std::runtime_error("Vulkan graph order changed node count");
            }
            auto ** reordered_array = ggml_graph_nodes(graph_);
            std::array<size_t, GGML_OP_COUNT> reordered_ops{};
            for (int i = 0; i < original_count; ++i) {
                moved_positions += original_nodes[i] != reordered_array[i];
                ++reordered_ops[reordered_array[i]->op];
            }
            if (original_ops != reordered_ops) {
                throw std::runtime_error("Vulkan graph order changed operation counts");
            }
            std::vector<ggml_tensor *> reordered_nodes(reordered_array, reordered_array + original_count);
            std::sort(original_nodes.begin(), original_nodes.end(), std::less<ggml_tensor *>{});
            std::sort(reordered_nodes.begin(), reordered_nodes.end(), std::less<ggml_tensor *>{});
            if (original_nodes != reordered_nodes) {
                throw std::runtime_error("Vulkan graph order changed node membership");
            }
        }
        gallocr_ = ggml_gallocr_new(ggml_backend_get_default_buffer_type(backend_));
        if (gallocr_ == nullptr || !ggml_gallocr_alloc_graph(gallocr_, graph_)) {
            throw std::runtime_error("failed to allocate HTDemucs graph buffer");
        }
        if (reorder_vulkan) {
            std::fprintf(stderr, "conflux_graph_order nodes=%d moved_positions=%zu graph_buffer_bytes=%zu\n",
                         ggml_graph_n_nodes(graph_), moved_positions, ggml_gallocr_get_buffer_size(gallocr_, 0));
        }
    }]=])
string(FIND "${graph_order_content}" "${graph_order_include_anchor}" graph_order_include_position)
string(FIND "${graph_order_content}" "${graph_order_finalize_anchor}" graph_order_finalize_position)
if(graph_order_include_position EQUAL -1 OR graph_order_finalize_position EQUAL -1)
    message(FATAL_ERROR "Pinned graph order include/finalize anchors were not found")
endif()
string(REPLACE "${graph_order_include_anchor}" "${graph_order_include_replacement}"
    graph_order_content "${graph_order_content}")
string(REPLACE "${graph_order_finalize_anchor}" "${graph_order_finalize_replacement}"
    graph_order_content "${graph_order_content}")
file(CONFIGURE OUTPUT "${phase_pipeline}" CONTENT "${graph_order_content}" @ONLY)
target_include_directories(engine_model_demucs BEFORE PRIVATE "${AUDIOCPP_SOURCE_DIR}/external/ggml/src")
message(STATUS "Conflux candidate: one Vulkan graph-order pass before allocation (stderr node/buffer diagnostics)")
