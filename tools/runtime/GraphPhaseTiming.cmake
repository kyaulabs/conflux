# SPDX-License-Identifier: AGPL-3.0-or-later

# Publish already-collected CPU wall timings; leave upstream sources and GPU
# synchronization unchanged, as with the blocking-wait build-directory overlay.
set(phase_header_source "${AUDIOCPP_SOURCE_DIR}/include/engine/models/demucs/pipeline.h")
set(phase_pipeline_source "${AUDIOCPP_SOURCE_DIR}/src/models/demucs/pipeline.cpp")
file(SHA256 "${phase_header_source}" phase_header_hash)
file(SHA256 "${phase_pipeline_source}" phase_pipeline_hash)
if(NOT phase_header_hash STREQUAL
        "1717f09faf110d121e5de0a55786b8cf983b647016c57f2db5f865931d7eeb7f"
        OR NOT phase_pipeline_hash STREQUAL
        "0da057a313fcf868c9e4be2fb27bd42d802a3f9ddd53cf98f0b55d9f98a52673")
    message(FATAL_ERROR "Graph phase timing requires the pinned audio.cpp v0.9.2 pipeline header and source")
endif()

file(READ "${phase_header_source}" phase_header_content)
file(READ "${phase_pipeline_source}" phase_pipeline_content)
set(phase_header_anchor "    double graph_rebuild_ms = 0.0;")
set(phase_header_fields [=[    double graph_rebuild_ms = 0.0;
    double graph_upload_ms = 0.0;
    double graph_compute_ms = 0.0;
    double graph_readback_ms = 0.0;]=])
set(phase_pipeline_anchor "    impl_->last_timing.graph_rebuild_ms = graph_timing.rebuild_ms;")
set(phase_pipeline_fields [=[    impl_->last_timing.graph_rebuild_ms = graph_timing.rebuild_ms;
    impl_->last_timing.graph_upload_ms = graph_timing.upload_ms;
    impl_->last_timing.graph_compute_ms = graph_timing.compute_ms;
    impl_->last_timing.graph_readback_ms = graph_timing.readback_ms;]=])
string(FIND "${phase_header_content}" "${phase_header_anchor}" phase_header_position)
string(FIND "${phase_pipeline_content}" "${phase_pipeline_anchor}" phase_pipeline_position)
if(phase_header_position EQUAL -1 OR phase_pipeline_position EQUAL -1)
    message(FATAL_ERROR "Pinned HTDemucs timing declarations or assignments were not found")
endif()
string(REPLACE "${phase_header_anchor}" "${phase_header_fields}"
    phase_header_content "${phase_header_content}")
string(REPLACE "${phase_pipeline_anchor}" "${phase_pipeline_fields}"
    phase_pipeline_content "${phase_pipeline_content}")

set(phase_directory "${CMAKE_CURRENT_BINARY_DIR}/conflux-graph-phase-timing")
set(phase_include_directory "${phase_directory}/include")
set(phase_header "${phase_include_directory}/engine/models/demucs/pipeline.h")
set(phase_pipeline "${phase_directory}/pipeline.cpp")
file(CONFIGURE OUTPUT "${phase_header}" CONTENT "${phase_header_content}" @ONLY)
file(CONFIGURE OUTPUT "${phase_pipeline}" CONTENT "${phase_pipeline_content}" @ONLY)

get_target_property(phase_model_sources engine_model_demucs SOURCES)
list(FIND phase_model_sources "src/models/demucs/pipeline.cpp" phase_source_index)
if(phase_source_index EQUAL -1)
    message(FATAL_ERROR "Pinned engine_model_demucs pipeline source was not found")
endif()
list(REMOVE_AT phase_model_sources ${phase_source_index})
list(APPEND phase_model_sources "${phase_pipeline}")
set_property(TARGET engine_model_demucs PROPERTY SOURCES "${phase_model_sources}")

# session.cpp and registry.cpp also include pipeline.h through session.h.
# Preinclude the same header in every affected target. If a quoted include ever
# resolves the original header, its duplicate declarations fail compilation.
target_include_directories(engine_model_demucs BEFORE PRIVATE "${phase_include_directory}")
target_include_directories(engine_runtime BEFORE PUBLIC "${phase_include_directory}")
target_include_directories(conflux-stem-runtime BEFORE PRIVATE "${phase_include_directory}")
foreach(phase_target IN ITEMS engine_model_demucs engine_runtime conflux-stem-runtime)
    target_compile_options(${phase_target} PRIVATE "SHELL:-include \"${phase_header}\"")
endforeach()
message(STATUS "Conflux HTDemucs graph phase timing: existing CPU wall clocks (upstream sources unchanged)")
