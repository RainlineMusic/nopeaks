cmake_minimum_required(VERSION 3.22)
project(NOPEAKS VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NOPEAKS_BUILD_PLUGINS "Build the Master and Track VST3 plug-ins and standalone apps" ON)
option(NOPEAKS_BUILD_TESTS "Build deterministic DSP, optimizer and transport tests" ON)
option(NOPEAKS_BUILD_HOST_TESTS "Build processor integration tests when plug-ins and tests are enabled" ON)
option(NOPEAKS_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
set(NOPEAKS_JUCE_PATH "" CACHE PATH "Existing JUCE checkout (otherwise download the pinned revision)")

find_package(Threads REQUIRED)
add_library(nopeaks_core STATIC
    src/core/Dsp.cpp
    src/core/Optimizer.cpp
    src/core/SharedBus.cpp)
target_include_directories(nopeaks_core PUBLIC include)
target_link_libraries(nopeaks_core PUBLIC Threads::Threads)
set_target_properties(nopeaks_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(nopeaks_core PUBLIC rt)
endif()
if(MSVC)
    target_compile_options(nopeaks_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(nopeaks_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(NOPEAKS_SANITIZE AND NOT MSVC)
    target_compile_options(nopeaks_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(nopeaks_core PUBLIC -fsanitize=address,undefined)
endif()

if(NOPEAKS_BUILD_TESTS)
    enable_testing()
    add_executable(nopeaks_tests tests/main.cpp tests/test_dsp.cpp tests/test_optimizer.cpp tests/test_shared_bus.cpp
        tests/test_coordinator.cpp tests/test_audio_quality.cpp)
    target_link_libraries(nopeaks_tests PRIVATE nopeaks_core)
    add_test(NAME nopeaks_core_tests COMMAND nopeaks_tests)
    set_tests_properties(nopeaks_core_tests PROPERTIES TIMEOUT 90)
endif()

if(NOPEAKS_BUILD_PLUGINS)
    if(NOPEAKS_JUCE_PATH)
        add_subdirectory("${NOPEAKS_JUCE_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        # JUCE 8.0.12. Pin the commit rather than a moving branch/tag.
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 29396c22c93392d6738e021b83196283d6e4d850
            GIT_SHALLOW FALSE)
        FetchContent_MakeAvailable(JUCE)
    endif()

    function(nopeaks_add_plugin target product code master)
        juce_add_plugin(${target}
            COMPANY_NAME "Rainline Music"
            COMPANY_WEBSITE "https://rainlinemusic.su"
            BUNDLE_ID "su.rainlinemusic.nopeaks.${code}"
            PLUGIN_MANUFACTURER_CODE RnLm
            PLUGIN_CODE ${code}
            PRODUCT_NAME "${product}"
            DESCRIPTION "Cooperative peak, spectral and stereo control"
            FORMATS VST3 Standalone
            IS_SYNTH FALSE
            NEEDS_MIDI_INPUT FALSE
            NEEDS_MIDI_OUTPUT FALSE
            IS_MIDI_EFFECT FALSE
            EDITOR_WANTS_KEYBOARD_FOCUS FALSE
            COPY_PLUGIN_AFTER_BUILD FALSE
            VST3_CATEGORIES Fx Dynamics EQ Tools)
        juce_generate_juce_header(${target})
        target_sources(${target} PRIVATE src/plugin/PluginProcessor.cpp src/plugin/PluginEditor.cpp)
        target_include_directories(${target} PRIVATE src/plugin include)
        target_compile_definitions(${target} PUBLIC
            NOPEAKS_IS_MASTER=${master}
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            JUCE_DISPLAY_SPLASH_SCREEN=0
            JUCE_REPORT_APP_USAGE=0)
        target_link_libraries(${target} PRIVATE
            nopeaks_core
            juce::juce_audio_utils
            juce::juce_dsp
            PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    endfunction()

    nopeaks_add_plugin(NOPEAKS_Master "NOPEAKS Master" NpMs 1)
    nopeaks_add_plugin(NOPEAKS_Track "NOPEAKS Track" NpTr 0)

    if(NOPEAKS_BUILD_TESTS AND NOPEAKS_BUILD_HOST_TESTS)
        # Reuse the compiled Master shared-code archive (including JUCE modules).
        # Explicit role constructors in the harness exercise Master and Track DSP.
        add_executable(nopeaks_host_tests tests/plugin_host.cpp)
        target_include_directories(nopeaks_host_tests PRIVATE src/plugin include "${JUCE_MODULES_DIR}")
        target_compile_definitions(nopeaks_host_tests PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
        target_link_libraries(nopeaks_host_tests PRIVATE NOPEAKS_Master)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            find_program(NOPEAKS_XVFB_RUN xvfb-run)
        endif()
        if(NOPEAKS_XVFB_RUN)
            add_test(NAME nopeaks_processor_tests COMMAND "${NOPEAKS_XVFB_RUN}" -a $<TARGET_FILE:nopeaks_host_tests>)
        else()
            add_test(NAME nopeaks_processor_tests COMMAND nopeaks_host_tests)
        endif()
        set_tests_properties(nopeaks_processor_tests PROPERTIES TIMEOUT 180)
    endif()
endif()
