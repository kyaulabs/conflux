cmake_minimum_required(VERSION 3.25)
project(CONFLUX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PipeWire REQUIRED IMPORTED_TARGET libpipewire-0.3>=0.3.65)
pkg_check_modules(SndFile REQUIRED IMPORTED_TARGET sndfile>=1.2)

add_library(conflux-audio src/audio/RenderCore.cpp src/audio/AudioFile.cpp)
target_include_directories(conflux-audio PUBLIC src)
target_link_libraries(conflux-audio PRIVATE PkgConfig::SndFile)
target_compile_options(conflux-audio PRIVATE -Wall -Wextra -Wpedantic)

add_executable(conflux-audio-probe src/probe_main.cpp)
target_link_libraries(conflux-audio-probe PRIVATE conflux-audio PkgConfig::PipeWire)
target_compile_options(conflux-audio-probe PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS conflux-audio-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    find_package(Threads REQUIRED)
    add_executable(conflux-audio-test tests/audio_test.cpp)
    target_link_libraries(conflux-audio-test PRIVATE conflux-audio)
    target_compile_options(conflux-audio-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME audio-render COMMAND conflux-audio-test)
    add_executable(conflux-audio-file-test tests/audio_file_test.cpp)
    target_link_libraries(conflux-audio-file-test PRIVATE conflux-audio)
    target_compile_options(conflux-audio-file-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME audio-file COMMAND conflux-audio-file-test)
    add_executable(conflux-audio-transport-test tests/audio_transport_test.cpp)
    target_link_libraries(conflux-audio-transport-test PRIVATE conflux-audio Threads::Threads)
    target_compile_options(conflux-audio-transport-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME audio-transport COMMAND conflux-audio-transport-test)
    set_tests_properties(audio-transport PROPERTIES TIMEOUT 15)
    add_test(NAME audio-probe-cli COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/audio_probe_cli_test.py $<TARGET_FILE:conflux-audio-probe>)
    set_tests_properties(audio-probe-cli PROPERTIES TIMEOUT 60)
    add_test(NAME probe-rejects-invalid-quantum COMMAND conflux-audio-probe --quantum 0)
    set_tests_properties(probe-rejects-invalid-quantum PROPERTIES WILL_FAIL TRUE)
    add_test(NAME stem-worker COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/stem_worker_test.py)
    set_tests_properties(stem-worker PROPERTIES
        ENVIRONMENT "PYTHONPYCACHEPREFIX=${CMAKE_BINARY_DIR}/pycache")
    add_test(NAME stem-soak COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/stem_soak_test.py)
    set_tests_properties(stem-soak PROPERTIES
        ENVIRONMENT "PYTHONPYCACHEPREFIX=${CMAKE_BINARY_DIR}/pycache")
endif()

option(CONFLUX_BUILD_UI "Build the Qt Quick runtime harness" ON)
if(CONFLUX_BUILD_UI)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Quick QuickControls2 Svg)
    qt_standard_project_setup()
    qt_add_executable(conflux src/main.cpp)
    target_link_libraries(conflux PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
    target_compile_options(conflux PRIVATE -Wall -Wextra -Wpedantic)
    qt_add_resources(conflux runtime-assets PREFIX "/" FILES
        qml/Main.qml
        brand/integration/qtquick/qmldir
        brand/integration/qtquick/ConfluxTheme.qml
        brand/assets/logos/conflux-horizontal-dark.svg
        brand/assets/logos/conflux-horizontal-light.svg
        brand/assets/app-icons/conflux-app-dark.svg
        brand/assets/fonts/Barlow-Regular.ttf
        brand/assets/fonts/JetBrainsMono-Regular.ttf
    )
    install(TARGETS conflux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/com.kyaulabs.conflux.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES brand/assets/app-icons/conflux-app-dark.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME com.kyaulabs.conflux.svg)
    install(FILES brand/assets/fonts/Barlow-OFL.txt brand/assets/fonts/JetBrainsMono-OFL.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/conflux/fonts)
endif()
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/conflux)
