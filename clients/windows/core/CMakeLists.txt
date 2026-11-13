cmake_minimum_required(VERSION 4.3)
project(BeamItWindowsCore LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(beamit_root "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

include(CTest)

if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/std:c++latest> $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
else()
    add_compile_options(-std=c++2c)
endif()

add_library(BeamItWindowsCore STATIC FrameProtocol.cpp)
target_include_directories(BeamItWindowsCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${beamit_root}/src"
    "${beamit_root}/include")

if(MSVC)
    target_compile_options(BeamItWindowsCore PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(BeamItWindowsCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    include("${beamit_root}/cmake/CPM.cmake")
    include("${beamit_root}/cmake/BeamItDependencies.cmake")
    beamit_add_network_dependencies()

    CPMAddPackage(
        NAME opus
        GIT_TAG v1.5.2
        GITHUB_REPOSITORY xiph/opus
        OPTIONS
            "BUILD_TESTING OFF"
            "OPUS_BUILD_TESTING OFF"
            "OPUS_BUILD_PROGRAMS OFF"
            "OPUS_INSTALL_PKG_CONFIG_MODULE OFF"
            "OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF"
            "OPUS_STATIC_RUNTIME OFF"
    )

    target_sources(BeamItWindowsCore PRIVATE
        AudioClient.cpp
        ClientProtocol.cpp
        ClientSession.cpp
        DecoderProbe.cpp
        HostWindows.cpp
        KeyCodes.cpp
        VideoPipeline.cpp
        "${beamit_root}/src/net/Crypto.cpp"
        "${beamit_root}/src/media/OpusAudio.cpp"
    )
    target_compile_definitions(BeamItWindowsCore PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(BeamItWindowsCore PUBLIC Boost::json beamit_wintls opus ws2_32 d3d11 dxgi mf mfplat mfuuid
        ole32 mmdevapi avrt winmm windowscodecs)
    add_executable(BeamItDecoderProbe DecoderProbeMain.cpp)
    target_link_libraries(BeamItDecoderProbe PRIVATE BeamItWindowsCore)

    # The WinUI MSBuild project consumes the core's include paths, definitions and static libraries from here.
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/BeamItWindowsCore.props" CONTENT [[<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>$<JOIN:$<REMOVE_DUPLICATES:$<TARGET_PROPERTY:BeamItWindowsCore,INTERFACE_INCLUDE_DIRECTORIES>;$<TARGET_PROPERTY:Boost::json,INTERFACE_INCLUDE_DIRECTORIES>;$<TARGET_PROPERTY:beamit_wintls,INTERFACE_INCLUDE_DIRECTORIES>;$<TARGET_PROPERTY:opus,INTERFACE_INCLUDE_DIRECTORIES>>,;>;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>$<JOIN:$<TARGET_PROPERTY:BeamItWindowsCore,INTERFACE_COMPILE_DEFINITIONS>,;>;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    </ClCompile>
    <Link>
      <AdditionalDependencies>$<TARGET_LINKER_FILE:BeamItWindowsCore>;$<TARGET_LINKER_FILE:Boost::json>;$<TARGET_LINKER_FILE:Boost::container>;$<TARGET_LINKER_FILE:opus>;ws2_32.lib;crypt32.lib;secur32.lib;bcrypt.lib;mmdevapi.lib;avrt.lib;winmm.lib;windowscodecs.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
</Project>
]])
endif()

if(BUILD_TESTING)
    add_executable(BeamItFrameProtocolTests ../tests/FrameProtocolTests.cpp)
    target_link_libraries(BeamItFrameProtocolTests PRIVATE BeamItWindowsCore)
    add_test(NAME BeamItFrameProtocolTests COMMAND BeamItFrameProtocolTests)

    if(WIN32)
        beamit_add_catch2()
        add_executable(BeamItClientTests
            ../tests/ClientProtocolTests.cpp
            ../tests/ClientSessionTests.cpp
            ../tests/HostWindowsTests.cpp
            ../tests/HostEndToEndTests.cpp
            ../tests/VideoPipelineTests.cpp
        )
        target_compile_definitions(BeamItClientTests PRIVATE
            BEAMIT_CLIENT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../build/fixtures")
        target_link_libraries(BeamItClientTests PRIVATE BeamItWindowsCore Catch2::Catch2WithMain)
        target_compile_options(BeamItClientTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /Zc:__cplusplus>)
        add_test(NAME BeamItClientTests COMMAND BeamItClientTests)
    endif()
endif()
