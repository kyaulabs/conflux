# SPDX-License-Identifier: AGPL-3.0-or-later
# Opt in to the measured HTDemucs Vulkan Flash candidate. Baseline stays default.
option(CONFLUX_STEM_FLASH_F32
    "Use the pinned Vulkan Flash attention path with F32 intermediates" OFF)
if(NOT CONFLUX_STEM_FLASH_F32)
    return()
endif()

if(NOT TARGET engine_model_demucs OR NOT TARGET ggml-vulkan
        OR NOT CONFLUX_STEM_BLOCKING_WAIT
        OR NOT DEFINED phase_pipeline OR NOT DEFINED phase_header
        OR NOT DEFINED blocking_source)
    message(FATAL_ERROR "Full-F32 Flash candidate requires phase timing and blocking-wait source copies")
endif()

function(conflux_flash_require_pin path expected)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Full-F32 Flash input is missing: ${path}")
    endif()
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Full-F32 Flash input hash mismatch: ${path}")
    endif()
endfunction()

# Bind the upstream interfaces and existing full-F32 shader, without editing any
# shader, device-wide feature flag, timing declaration or upstream source.
foreach(flash_pin IN ITEMS
        "src/models/demucs/pipeline.cpp|0da057a313fcf868c9e4be2fb27bd42d802a3f9ddd53cf98f0b55d9f98a52673"
        "include/engine/models/demucs/pipeline.h|1717f09faf110d121e5de0a55786b8cf983b647016c57f2db5f865931d7eeb7f"
        "external/ggml/include/ggml.h|7177761cb4ad6b20e4699f6aa8814c4f7a9cdc6ce549355a97ca91e421b36716"
        "external/ggml/include/ggml-backend.h|0389a9d79c65a83567b49017866146228f3559d5971af61ae95d6071b7066f17"
        "external/ggml/include/ggml-vulkan.h|0b6f625021b047907d24b60b3ca441e8a97d44235f0d32abeb2665112c35418e"
        "external/ggml/include/ggml-alloc.h|ab9a5572847a98ec31ee57fe202f38216be01da3e63e36183d7d8b9f34756a0f"
        "external/ggml/src/ggml-vulkan/ggml-vulkan.cpp|e58a915da3620150a33cf8ac5cc4f454206d5c51221860e5fc9db91d7b590d1d"
        "external/ggml/src/ggml-vulkan/vulkan-shaders/vulkan-shaders-gen.cpp|a63b5c59d3973b153b78a2b49e33da84dd714f221564a0c901cc6c9be5024791"
        "external/ggml/src/ggml-vulkan/vulkan-shaders/flash_attn.comp|8185cd8b80c435545bbc0b06d7d2a1d2225640a2e8cf6564ec03994651e8c5fc"
        "external/ggml/src/ggml-vulkan/vulkan-shaders/flash_attn_base.glsl|8a47fdfb4ecb7070925b266674aad576cf5aa6743b25baca977017102f3d1c51"
        "external/ggml/src/ggml-vulkan/vulkan-shaders/flash_attn_dequant.glsl|0d368f5cff0c216259ded67bb1aa43be4a320e86a8782f53807017112bff4b92"
        "external/ggml/src/ggml-vulkan/vulkan-shaders/types.glsl|a87a43cbca5db18230dd36cf9e230891f0af08519c85ba5e3d991e425e4278c5")
    string(REPLACE "|" ";" flash_pin_parts "${flash_pin}")
    list(GET flash_pin_parts 0 flash_pin_path)
    list(GET flash_pin_parts 1 flash_pin_hash)
    conflux_flash_require_pin("${AUDIOCPP_SOURCE_DIR}/${flash_pin_path}" "${flash_pin_hash}")
endforeach()
conflux_flash_require_pin("${CMAKE_CURRENT_SOURCE_DIR}/GraphPhaseTiming.cmake"
    "91891a8a2dc01be4cb02a835d0c83f1b8b41a98da9059a36021c4a3b1e38d524")
conflux_flash_require_pin("${CMAKE_CURRENT_SOURCE_DIR}/BlockingVulkanWait.cmake"
    "4ae34a1fee27c3fec3b5a4b9884a0c86fe7395acabf9d7c2006345e56384ddb3")
conflux_flash_require_pin("${CMAKE_CURRENT_SOURCE_DIR}/htdemucs_segment.cpp"
    "a00c5c84bf57adeea7aa21eea992da4ee50d219fbc6994180de4dbf9fd03d7d9")
conflux_flash_require_pin("${phase_pipeline}"
    "f06814211f00eab41487caacefe1c2492dfe1d2a0ddd6da1f90bdbcc566fc8f9")
conflux_flash_require_pin("${phase_header}"
    "7f48bbd4964ebcce721f7544a84303c568b460bf21c24090899ccc8b983833cd")
conflux_flash_require_pin("${blocking_source}"
    "55629ce73c55ab1d1513408c6276d9a0de6adfaac1531922fdb256aba5be0209")

get_target_property(flash_model_sources engine_model_demucs SOURCES)
get_target_property(flash_vulkan_sources ggml-vulkan SOURCES)
list(FIND flash_model_sources "${phase_pipeline}" flash_model_index)
list(FIND flash_vulkan_sources "${blocking_source}" flash_vulkan_index)
if(flash_model_index EQUAL -1 OR flash_vulkan_index EQUAL -1)
    message(FATAL_ERROR "Full-F32 Flash expected source copies are not target sources")
endif()

file(READ "${phase_pipeline}" flash_pipeline_content)
file(READ "${blocking_source}" flash_vulkan_content)
set(flash_include_anchor "#include <ggml-backend.h>")
set(flash_include_replacement [=[#include <ggml-backend.h>
#include <ggml-vulkan.h>
#include <cstdio>
#include <cstring>]=])
set(flash_gate_anchor "if (ctx.backend_type == core::BackendType::Cuda && flash_attn_supports_head_dim(head_dim)) {")
set(flash_gate_replacement "if ((ctx.backend_type == core::BackendType::Cuda || ctx.backend_type == core::BackendType::Vulkan) && flash_attn_supports_head_dim(head_dim)) {")
set(flash_finalize_anchor [=[        gallocr_ = ggml_gallocr_new(ggml_backend_get_default_buffer_type(backend_));]=])
set(flash_finalize_replacement [=[        if (ggml_backend_is_vk(backend_)) {
            int signatures[4] = {0, 0, 0, 0};
            int flash_count = 0;
            for (int i = 0; i < ggml_graph_n_nodes(graph_); ++i) {
                const auto * node = ggml_graph_node(graph_, i);
                if (node->op != GGML_OP_FLASH_ATTN_EXT) {
                    continue;
                }
                const auto * q = node->src[0];
                const auto * k = node->src[1];
                const auto * v = node->src[2];
                const auto valid_input = [](const ggml_tensor * value) {
                    return value != nullptr && value->type == GGML_TYPE_F32 &&
                        value->ne[0] == 64 && (value->ne[1] == 1344 || value->ne[1] == 2688) &&
                        value->ne[2] == 8 && value->ne[3] == 1 && ggml_is_contiguous(value);
                };
                // The existing Flash branch materializes q_heads/k_heads/v_heads
                // with ensure_backend_addressable_layout before constructing this op.
                if (!valid_input(q) || !valid_input(k) || !valid_input(v) ||
                    k->ne[1] != v->ne[1] || node->type != GGML_TYPE_F32 ||
                    node->ne[0] != 64 || node->ne[1] != 8 || node->ne[2] != q->ne[1] ||
                    node->ne[3] != 1 || !ggml_is_contiguous(node) ||
                    node->src[3] != nullptr || node->src[4] != nullptr ||
                    ggml_flash_attn_ext_get_prec(node) != GGML_PREC_F32) {
                    throw std::runtime_error("Full-F32 Flash candidate tensor geometry/precision mismatch");
                }
                float params[3];
                std::memcpy(params, node->op_params, sizeof(params));
                if (params[0] != 0.125f || params[1] != 0.0f || params[2] != 0.0f ||
                    !ggml_backend_supports_op(backend_, node)) {
                    throw std::runtime_error("Full-F32 Flash candidate parameters or Vulkan support rejected");
                }
                const int signature = q->ne[1] == k->ne[1]
                    ? (q->ne[1] == 1344 ? 0 : 1)
                    : (q->ne[1] == 2688 ? 2 : 3);
                ++signatures[signature];
                ++flash_count;
                std::fprintf(stderr,
                    "conflux_flash_f32_node index=%d q=64,%lld,8,1 k=64,%lld,8,1 v=64,%lld,8,1 out=64,8,%lld,1 scale=0.125 precision=f32 supported=1\n",
                    i, static_cast<long long>(q->ne[1]), static_cast<long long>(k->ne[1]),
                    static_cast<long long>(v->ne[1]), static_cast<long long>(q->ne[1]));
            }
            if (flash_count != 10 || signatures[0] != 3 || signatures[1] != 3 ||
                signatures[2] != 2 || signatures[3] != 2) {
                throw std::runtime_error("Full-F32 Flash candidate requires exact ten-node Demucs attention signatures");
            }
            std::fprintf(stderr, "conflux_flash_f32_graph nodes=%d flash_nodes=%d self1344=3 self2688=3 cross2688to1344=2 cross1344to2688=2\n",
                         ggml_graph_n_nodes(graph_), flash_count);
        }
        gallocr_ = ggml_gallocr_new(ggml_backend_get_default_buffer_type(backend_));]=])

# F32 K/V plus F32 accumulation is the existing pipeline-state key; Q and output
# are already required F32 by Vulkan Flash support. On non-FP16 devices default
# precision also has f32acc=true, where the existing full-F32 shader was used.
# All ten candidate Demucs nodes above explicitly request GGML_PREC_F32.
set(flash_tuning_anchor [=[static vk_fa_tuning_params get_fa_tuning_params(const vk_device& device, uint32_t hsk, uint32_t hsv, uint32_t n_rows, uint32_t n_kv, ggml_type k_type, ggml_type v_type, bool f32acc) {
    FaCodePath path = device->coopmat2 ? FA_COOPMAT2 :]=])
set(flash_tuning_replacement [=[static vk_fa_tuning_params get_fa_tuning_params(const vk_device& device, uint32_t hsk, uint32_t hsv, uint32_t n_rows, uint32_t n_kv, ggml_type k_type, ggml_type v_type, bool f32acc) {
    if (f32acc && k_type == GGML_TYPE_F32 && v_type == GGML_TYPE_F32) {
        const auto params = get_fa_tuning_params_scalar(device, hsk, hsv, n_rows, n_kv, k_type, v_type, f32acc);
        if (!ggml_vk_flash_attn_scalar_shmem_support(device, params, hsk, hsv, f32acc, k_type, v_type)) {
            GGML_ABORT("Full-F32 Flash scalar shared memory is unsupported");
        }
        return params;
    }
    FaCodePath path = device->coopmat2 ? FA_COOPMAT2 :]=])
set(flash_shmem_anchor [=[static bool ggml_vk_flash_attn_scalar_shmem_support(const vk_device& device, const vk_fa_tuning_params& params, uint32_t hsk, uint32_t hsv, bool f32acc, ggml_type k_type, ggml_type v_type) {
    GGML_UNUSED(f32acc);
    GGML_UNUSED(v_type);
    // Needs to be kept up to date on shader changes
    const uint32_t wg_size = params.workgroup_size;
    const uint32_t Br = params.block_rows;
    const uint32_t Bc = params.block_cols;

    const uint32_t float_type_size = device->fp16 ? sizeof(ggml_fp16_t) : sizeof(float);]=])
set(flash_shmem_replacement [=[static bool ggml_vk_flash_attn_scalar_shmem_support(const vk_device& device, const vk_fa_tuning_params& params, uint32_t hsk, uint32_t hsv, bool f32acc, ggml_type k_type, ggml_type v_type) {
    // Match the full-F32 scalar shader only for this F32 K/V precision key.
    const bool full_f32 = f32acc && k_type == GGML_TYPE_F32 && v_type == GGML_TYPE_F32;
    const uint32_t wg_size = params.workgroup_size;
    const uint32_t Br = params.block_rows;
    const uint32_t Bc = params.block_cols;

    const uint32_t float_type_size = (device->fp16 && !full_f32) ? sizeof(ggml_fp16_t) : sizeof(float);]=])
set(flash_shader_anchor [=[        } else {
            if (device->fp16) {
                if (f32acc) { spv_data = flash_attn_f32_f16_data;        spv_size = flash_attn_f32_f16_len; }
                else        { spv_data = flash_attn_f32_f16_f16acc_data; spv_size = flash_attn_f32_f16_f16acc_len; }
            } else {
                spv_data = flash_attn_f32_f16_fp32_data;
                spv_size = flash_attn_f32_f16_fp32_len;
            }
        }
        const char *name = aligned ? "flash_attn_f32_f16_aligned" : "flash_attn_f32_f16";]=])
set(flash_shader_replacement [=[        } else {
            const bool full_f32 = f32acc && fa.first.k_type == GGML_TYPE_F32 && fa.first.v_type == GGML_TYPE_F32;
            if (full_f32 || !device->fp16) {
                spv_data = flash_attn_f32_f16_fp32_data;
                spv_size = flash_attn_f32_f16_fp32_len;
                if (full_f32 && !fa.second->initialized) {
                    fprintf(stderr,
                        "conflux_flash_f32_pipeline shader=flash_attn_f32_f16_fp32 path=scalar intermediate_bytes=4 accumulation_bytes=4 hsk=%u hsv=%u Br=%u Bc=%u workgroup=%u subgroup=%u k=f32 v=f32\n",
                        fa.first.HSK, fa.first.HSV, Br, Bc, fa.first.workgroup_size, fa.first.subgroup_size);
                }
            } else {
                if (f32acc) { spv_data = flash_attn_f32_f16_data;        spv_size = flash_attn_f32_f16_len; }
                else        { spv_data = flash_attn_f32_f16_f16acc_data; spv_size = flash_attn_f32_f16_f16acc_len; }
            }
        }
        const char *name = aligned ? "flash_attn_f32_f16_aligned" : "flash_attn_f32_f16";]=])

# Exact pinned inputs plus exact anchor multiplicities reject repeated overlays
# and accidental wider replacements, before writing either candidate source.
foreach(flash_change IN ITEMS include gate finalize tuning shmem shader)
    if(flash_change STREQUAL "include" OR flash_change STREQUAL "gate" OR flash_change STREQUAL "finalize")
        set(flash_content_var flash_pipeline_content)
    else()
        set(flash_content_var flash_vulkan_content)
    endif()
    set(flash_expected_count 1)
    if(flash_change STREQUAL "gate")
        set(flash_expected_count 2)
    endif()
    set(flash_anchor "${flash_${flash_change}_anchor}")
    string(LENGTH "${${flash_content_var}}" flash_before_length)
    string(LENGTH "${flash_anchor}" flash_anchor_length)
    string(REPLACE "${flash_anchor}" "" flash_without_anchor "${${flash_content_var}}")
    string(LENGTH "${flash_without_anchor}" flash_after_length)
    math(EXPR flash_anchor_count "(${flash_before_length} - ${flash_after_length}) / ${flash_anchor_length}")
    if(NOT flash_anchor_count EQUAL flash_expected_count)
        message(FATAL_ERROR "Full-F32 Flash pinned ${flash_change} anchor count is not ${flash_expected_count}")
    endif()
    string(REPLACE "${flash_anchor}" "${flash_${flash_change}_replacement}"
        ${flash_content_var} "${${flash_content_var}}")
endforeach()

set(flash_directory "${CMAKE_CURRENT_BINARY_DIR}/conflux-flash-f32")
set(flash_pipeline "${flash_directory}/pipeline.cpp")
set(flash_vulkan "${flash_directory}/ggml-vulkan.cpp")
file(CONFIGURE OUTPUT "${flash_pipeline}" CONTENT "${flash_pipeline_content}" @ONLY)
file(CONFIGURE OUTPUT "${flash_vulkan}" CONTENT "${flash_vulkan_content}" @ONLY)
list(REMOVE_AT flash_model_sources ${flash_model_index})
list(APPEND flash_model_sources "${flash_pipeline}")
set_property(TARGET engine_model_demucs PROPERTY SOURCES "${flash_model_sources}")
list(REMOVE_AT flash_vulkan_sources ${flash_vulkan_index})
list(APPEND flash_vulkan_sources "${flash_vulkan}")
set_property(TARGET ggml-vulkan PROPERTY SOURCES "${flash_vulkan_sources}")
message(STATUS "Conflux numerical candidate: ten Vulkan Flash nodes; existing scalar full-F32 shader; no graph reorder")
