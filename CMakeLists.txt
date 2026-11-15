cmake_minimum_required(VERSION 3.22)

# Mac : binaire universel (Intel + Apple Silicon), à définir avant project()
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")

project(OniriqueHalo VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.6
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

option(HALO_PLUGIN "Compiler le plugin" ON)
option(HALO_TESTS "Compiler le banc de mesure" ON)
# Licence ONIRIQUE : toujours dans les builds publiés. OFF seulement pour un build d'essai local.
option(HALO_LICENCE "Brancher la licence ONIRIQUE" ON)
# Build local de captures et de démo (jamais publié) : ONIRIQUE_UI_TEST=nolicense,demo
option(ONIRIQUE_UI_TEST "Build de captures et de démo" OFF)

set(HALO_FONTS License/Fonts/Unbounded800.ttf License/Fonts/HankenGrotesk600.ttf
               License/Fonts/JetBrainsMono500.ttf License/Fonts/JetBrainsMono600.ttf)

if(HALO_PLUGIN)
    set(HALO_FORMATS VST3)
    if(APPLE)
        list(APPEND HALO_FORMATS AU)
    endif()

    juce_add_plugin(OniriqueHalo
        COMPANY_NAME                Onirique
        PLUGIN_MANUFACTURER_CODE    Onir
        PLUGIN_CODE                 Ohal
        FORMATS                     ${HALO_FORMATS}
        AU_MAIN_TYPE                kAudioUnitType_Effect
        PRODUCT_NAME                "ONIRIQUE HALO"
        IS_SYNTH                    FALSE
        NEEDS_MIDI_INPUT            FALSE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        VST3_CATEGORIES             Fx Dynamics
        COPY_PLUGIN_AFTER_BUILD     FALSE)

    target_sources(OniriqueHalo PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

    juce_add_binary_data(HaloAssets SOURCES Resources/logo.png)
    set_target_properties(HaloAssets PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

    target_include_directories(OniriqueHalo PRIVATE Source ${CMAKE_CURRENT_SOURCE_DIR})

    if(HALO_LICENCE)
        include(License/OniriqueLicense.cmake)
        onirique_add_license(OniriqueHalo)
        target_compile_definitions(OniriqueHalo PRIVATE HALO_LICENCE=1)
    else()
        juce_add_binary_data(OniriqueHaloLicenseData
            HEADER_NAME OniriqueLicenseData.h
            NAMESPACE OniriqueLicenseData
            SOURCES ${HALO_FONTS})
        set_target_properties(OniriqueHaloLicenseData PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
        target_link_libraries(OniriqueHalo PRIVATE OniriqueHaloLicenseData)
        target_compile_definitions(OniriqueHalo PRIVATE HALO_LICENCE=0)
    endif()

    if(ONIRIQUE_UI_TEST)
        target_compile_definitions(OniriqueHalo PRIVATE ONIRIQUE_UI_TEST=1)
    endif()

    target_compile_definitions(OniriqueHalo PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    if(MSVC)
        target_compile_options(OniriqueHalo PRIVATE /utf-8)
    endif()
    if(APPLE)
        target_compile_definitions(OniriqueHalo PUBLIC JUCE_COREGRAPHICS_RENDER_WITH_MULTIPLE_PAINT_CALLS=1)
    endif()

    target_link_libraries(OniriqueHalo
        PRIVATE
            juce::juce_audio_utils
            juce::juce_dsp
            HaloAssets
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()

# Banc de mesure du moteur (console) : latences mesurées, nul, bande, delta, temps adaptatifs, sidechain,
# PARALIMIT, clics, stabilité, CPU
if(HALO_TESTS)
    juce_add_console_app(HaloBench PRODUCT_NAME "HaloBench")
    target_sources(HaloBench PRIVATE Tests/HaloBench.cpp)
    target_include_directories(HaloBench PRIVATE Source)
    target_compile_definitions(HaloBench PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    if(MSVC)
        target_compile_options(HaloBench PRIVATE /utf-8)
    endif()
    target_link_libraries(HaloBench PRIVATE juce::juce_dsp juce::juce_audio_basics
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
