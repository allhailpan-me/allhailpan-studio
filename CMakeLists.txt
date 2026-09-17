cmake_minimum_required(VERSION 3.22)
cmake_policy(SET CMP0091 NEW)   # lets us choose the MSVC runtime

project(AllHailPanStudio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE is downloaded automatically the first time you configure.
# For reproducible builds, change GIT_TAG to a specific release tag
# from https://github.com/juce-framework/JUCE/releases
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# Rubber Band Library (GPL) for high-quality time-stretching and pitch-shifting.
# We compile its single-file build straight into the app, so nothing else is needed.
FetchContent_Declare(
    rubberband
    GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
    GIT_TAG        v4.0.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(rubberband)
if(NOT rubberband_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)   # allow FetchContent_Populate on newer CMake
    endif()
    FetchContent_Populate(rubberband)
endif()
set(AHP_RUBBERBAND_SOURCE "${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp")

# Optional: path to the Steinberg ASIO SDK (Windows only) for low-latency drivers.
set(AHP_ASIO_SDK_DIR "" CACHE PATH "Folder containing the ASIO SDK (the one with common/iasiodrv.h)")

juce_add_gui_app(AllHailPanStudio
    PRODUCT_NAME "ALLHAILPAN Studio"
    COMPANY_NAME "AllHailPan"
    BUNDLE_ID    com.allhailpan.studio
    VERSION      ${PROJECT_VERSION}
    ICON_BIG     "${CMAKE_CURRENT_SOURCE_DIR}/Assets/app_icon.png"
)

# Release builds should run on any machine, without Visual Studio or the
# Microsoft runtime installed, so link the C++ runtime into the .exe.
if(MSVC)
    set_target_properties(AllHailPanStudio PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(AllHailPanStudio PRIVATE $<$<CONFIG:Release>:/O2>)
endif()

target_sources(AllHailPanStudio PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/AudioEngine.cpp
    Source/PluginManager.cpp
    Source/PlaylistComponent.cpp
    Source/PianoRollComponent.cpp
    Source/StretchCache.cpp
    Source/ProjectIO.cpp
    Source/DarkTitleBarWin.cpp
    ${AHP_RUBBERBAND_SOURCE}
)

target_include_directories(AllHailPanStudio PRIVATE "${rubberband_SOURCE_DIR}")
set_source_files_properties(${AHP_RUBBERBAND_SOURCE} PROPERTIES
    COMPILE_DEFINITIONS "NOMINMAX;_USE_MATH_DEFINES"
    SKIP_PRECOMPILE_HEADERS ON)
if(MSVC)
    set_source_files_properties(${AHP_RUBBERBAND_SOURCE} PROPERTIES COMPILE_OPTIONS "/W0;/bigobj")
else()
    set_source_files_properties(${AHP_RUBBERBAND_SOURCE} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Logos baked into the app (BinaryData.h)
juce_add_binary_data(AhpAssets SOURCES
    Assets/ahp_logo.png
    Assets/logo_spin.png
)

target_compile_definitions(AllHailPanStudio PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AllHailPanStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AllHailPanStudio,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
)

# Plugin formats and audio systems per platform
if(APPLE)
    target_compile_definitions(AllHailPanStudio PRIVATE JUCE_PLUGINHOST_AU=1)
endif()
if(UNIX AND NOT APPLE)
    target_compile_definitions(AllHailPanStudio PRIVATE JUCE_PLUGINHOST_LV2=1 JUCE_JACK=1)
endif()

# Dark title bars on Windows
if(WIN32)
    target_link_libraries(AllHailPanStudio PRIVATE dwmapi)
endif()

# ASIO (Windows only, optional)
if(WIN32 AND AHP_ASIO_SDK_DIR)
    if(EXISTS "${AHP_ASIO_SDK_DIR}/common/iasiodrv.h")
        target_include_directories(AllHailPanStudio PRIVATE "${AHP_ASIO_SDK_DIR}/common")
        target_compile_definitions(AllHailPanStudio PRIVATE JUCE_ASIO=1)
        message(STATUS "ASIO support enabled from ${AHP_ASIO_SDK_DIR}")
    else()
        # A missing or moved SDK shouldn't stop the build: carry on without ASIO
        message(WARNING "No ASIO SDK at ${AHP_ASIO_SDK_DIR} - building without ASIO. "
                        "Set AHP_ASIO_SDK_DIR to the folder containing common/iasiodrv.h to re-enable it.")
    endif()
endif()

target_link_libraries(AllHailPanStudio
    PRIVATE
        AhpAssets
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
