cmake_minimum_required(VERSION 4.3)
project(BeamItWindowsCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

include(CTest)

add_library(BeamItWindowsCore STATIC FrameProtocol.cpp)
target_include_directories(BeamItWindowsCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(BeamItWindowsCore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../src")

if(MSVC)
    target_compile_options(BeamItWindowsCore PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(BeamItWindowsCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_sources(BeamItWindowsCore PRIVATE DecoderProbe.cpp)
    target_compile_definitions(BeamItWindowsCore PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(BeamItWindowsCore PUBLIC d3d11 dxgi mf mfplat mfuuid ole32)
    add_executable(BeamItDecoderProbe DecoderProbeMain.cpp)
    target_link_libraries(BeamItDecoderProbe PRIVATE BeamItWindowsCore)
endif()

if(BUILD_TESTING)
    add_executable(BeamItFrameProtocolTests ../tests/FrameProtocolTests.cpp)
    target_include_directories(BeamItFrameProtocolTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../src")
    target_link_libraries(BeamItFrameProtocolTests PRIVATE BeamItWindowsCore)
    add_test(NAME BeamItFrameProtocolTests COMMAND BeamItFrameProtocolTests)
endif()
