# SPDX-License-Identifier: AGPL-3.0-or-later
enable_language(C)
include(FetchContent)
find_program(CONFLUX_PATCH_EXECUTABLE patch REQUIRED)
pkg_check_modules(FFTW REQUIRED IMPORTED_TARGET fftw3)

function(conflux_add_private_dsp)
    set(BUILD_SHARED_LIBS ON)
    set(BUILD_TESTING OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.13)
    set(patches "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")

    FetchContent_Declare(conflux_samplerate
        URL https://codeload.github.com/libsndfile/libsamplerate/tar.gz/c96f5e3de9c4488f4e6c97f59f5245f22fda22f7
        URL_HASH SHA256=35ceb3e63eb8884ec802b5be685a9aacf2458c4eb3c0ea38dc18ef1b00648c44
        PATCH_COMMAND "${CONFLUX_PATCH_EXECUTABLE}" --batch --fuzz=0 -p1 -i "${patches}/samplerate.patch")
    FetchContent_Declare(conflux_rubberband
        URL https://codeload.github.com/breakfastquay/rubberband/tar.gz/refs/tags/v4.0.0
        URL_HASH SHA256=24300f48a8014b7c863b573a9647e61b1b19b37875e2cdd92005e64c6424d266
        PATCH_COMMAND "${CONFLUX_PATCH_EXECUTABLE}" --batch --fuzz=0 -p1 -i "${patches}/rubberband.patch")
    # Upstream libsamplerate CMake config uses these during this configure only.
    set(LIBSAMPLERATE_EXAMPLES OFF)
    set(LIBSAMPLERATE_INSTALL OFF)
    FetchContent_MakeAvailable(conflux_samplerate conflux_rubberband)
    set_property(TARGET samplerate PROPERTY PUBLIC_HEADER "")
    target_compile_options(samplerate PRIVATE
        "$<$<C_COMPILER_ID:GNU,Clang>:-ffp-contract=off>")

    set(rb "${conflux_rubberband_SOURCE_DIR}")
    add_library(conflux-rubberband SHARED
        "${rb}/src/rubberband-c.cpp"
        "${rb}/src/RubberBandStretcher.cpp" "${rb}/src/RubberBandLiveShifter.cpp"
        "${rb}/src/faster/AudioCurveCalculator.cpp" "${rb}/src/faster/CompoundAudioCurve.cpp"
        "${rb}/src/faster/HighFrequencyAudioCurve.cpp" "${rb}/src/faster/SilentAudioCurve.cpp"
        "${rb}/src/faster/PercussiveAudioCurve.cpp" "${rb}/src/faster/R2Stretcher.cpp"
        "${rb}/src/faster/StretcherChannelData.cpp" "${rb}/src/faster/StretcherProcess.cpp"
        "${rb}/src/common/Allocators.cpp" "${rb}/src/common/FFT.cpp"
        "${rb}/src/common/Log.cpp" "${rb}/src/common/Profiler.cpp"
        "${rb}/src/common/Resampler.cpp" "${rb}/src/common/StretchCalculator.cpp"
        "${rb}/src/common/sysutils.cpp" "${rb}/src/common/mathmisc.cpp"
        "${rb}/src/common/Thread.cpp"
        "${rb}/src/finer/R3Stretcher.cpp" "${rb}/src/finer/R3LiveShifter.cpp")
    set_target_properties(conflux-rubberband PROPERTIES
        CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
        OUTPUT_NAME rubberband VERSION 3.0.0 SOVERSION 3 INSTALL_RPATH "\$ORIGIN")
    target_include_directories(conflux-rubberband PUBLIC "${rb}")
    target_compile_definitions(conflux-rubberband PRIVATE
        HAVE_FFTW3 FFTW_DOUBLE_ONLY HAVE_LIBSAMPLERATE HAVE_POSIX_MEMALIGN USE_PTHREADS)
    target_link_libraries(conflux-rubberband PRIVATE PkgConfig::FFTW samplerate Threads::Threads)

    install(TARGETS samplerate conflux-rubberband
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/conflux" NAMELINK_SKIP)
    install(FILES "${conflux_samplerate_SOURCE_DIR}/COPYING"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/conflux" RENAME libsamplerate-COPYING)
    install(FILES "${rb}/COPYING"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/conflux" RENAME rubberband-COPYING)
endfunction()
