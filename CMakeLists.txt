cmake_minimum_required(VERSION 4.3)
project(BeamIt)

set(CMAKE_CXX_STANDARD 26)

include(CTest)

include(cmake/CPM.cmake)
include(cmake/NvidiaNvenc.cmake)

CPMAddPackage(
    NAME spdlog
    GIT_TAG v1.15.3
    GITHUB_REPOSITORY gabime/spdlog
    OPTIONS
        "SPDLOG_FMT_EXTERNAL OFF"
)

CPMAddPackage(
    NAME CLI11
    GIT_TAG v2.7.2
    GITHUB_REPOSITORY CLIUtils/CLI11
    OPTIONS
        "CLI11_BUILD_TESTS OFF"
        "CLI11_BUILD_EXAMPLES OFF"
        "CLI11_BUILD_DOCS OFF"
)

CPMAddPackage(
    NAME Boost
    VERSION 1.90.0
    URL https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.xz
    URL_HASH SHA256=aca59f889f0f32028ad88ba6764582b63c916ce5f77b31289ad19421a96c555f
    OPTIONS
        "BOOST_INCLUDE_LIBRARIES beast\\\;json"
        "BOOST_ENABLE_CMAKE ON"
        "BUILD_TESTING OFF"
        "BOOST_BEAST_BUILD_TESTS OFF"
        "BOOST_BEAST_BUILD_EXAMPLES OFF"
        "BOOST_BEAST_BUILD_FUZZERS OFF"
)

# Header-only Schannel TLS stream for Asio/Beast; its own CMake expects a git checkout, so wrap it here.
CPMAddPackage(
    NAME wintls
    VERSION 1.0.0
    URL https://github.com/laudrup/boost-wintls/archive/refs/tags/v1.0.0.tar.gz
    URL_HASH SHA256=e5aa70d508ad709f25d6d6a1802e956b5706fe2d2761b619303ef6a851267e69
    DOWNLOAD_ONLY YES
)
add_library(beamit_wintls INTERFACE)
target_include_directories(beamit_wintls SYSTEM INTERFACE "${wintls_SOURCE_DIR}/include")
target_link_libraries(beamit_wintls INTERFACE Boost::beast crypt32 secur32 bcrypt)

set(BEAMIT_HOST_SOURCES
    src/host/HostServer.cpp
    src/host/StreamPipeline.cpp
    src/host/TlsIdentity.cpp
    src/media/BitstreamInfo.cpp
    src/net/Crypto.cpp
)

add_executable(BeamIt
    src/main.cpp
    src/ApplicationWindows.cpp
    src/capture/DxgiCapturer.cpp
    src/capture/WgcCapturer.cpp
    src/media/D3D12VideoEncoder.cpp
    src/media/AutoVideoEncoder.cpp
    src/media/CodecHeaders.cpp
    src/media/ProbeEncoders.cpp
    ${BEAMIT_HOST_SOURCES}
)
target_include_directories(BeamIt PRIVATE include src)
target_link_libraries(BeamIt PRIVATE spdlog CLI11::CLI11 Boost::beast Boost::json beamit_wintls
    d3d11 d3d12 dxgi windowsapp ws2_32 user32)
target_compile_definitions(BeamIt PUBLIC NOMINMAX _WIN32_WINNT=0x0A00)
if(TARGET NVIDIA::NVENCHeaders)
    target_sources(BeamIt PRIVATE src/media/ProbeNvenc.cpp src/media/NvencVideoEncoder.cpp)
    target_link_libraries(BeamIt PRIVATE NVIDIA::NVENCHeaders)
    target_compile_definitions(BeamIt PRIVATE BEAMIT_HAS_NVENC_PROBE=1)
endif()

if(BUILD_TESTING)
    CPMAddPackage(
        NAME Catch2
        GIT_TAG v3.7.1
        GITHUB_REPOSITORY catchorg/Catch2
        OPTIONS
            "CATCH_BUILD_TESTING OFF"
            "CATCH_BUILD_EXAMPLES OFF"
            "CATCH_INSTALL_DOCS OFF"
            "CATCH_INSTALL_EXTRAS OFF"
    )

    add_executable(BeamItTests
        tests/CaptureTypesTests.cpp
        tests/CodecHeadersTests.cpp
        tests/BeastTests.cpp
        tests/ProtocolTests.cpp
        src/ApplicationWindows.cpp
        ${BEAMIT_HOST_SOURCES}
        src/capture/DxgiCapturer.cpp
        src/capture/WgcCapturer.cpp
        src/media/D3D12VideoEncoder.cpp
        src/media/AutoVideoEncoder.cpp
        src/media/CodecHeaders.cpp
    )
    target_include_directories(BeamItTests PRIVATE include src)
    target_compile_definitions(BeamItTests PRIVATE BEAMIT_USE_CATCH2=1 NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(BeamItTests PRIVATE Catch2::Catch2WithMain spdlog Boost::beast Boost::json beamit_wintls
        d3d11 d3d12 dxgi windowsapp ws2_32 user32)
    if(TARGET NVIDIA::NVENCHeaders)
        target_sources(BeamItTests PRIVATE tests/NvencVideoEncoderTests.cpp src/media/NvencVideoEncoder.cpp)
        target_link_libraries(BeamItTests PRIVATE NVIDIA::NVENCHeaders)
        target_compile_definitions(BeamItTests PRIVATE BEAMIT_HAS_NVENC_PROBE=1)
    endif()
    add_test(NAME BeamItTests COMMAND BeamItTests)
endif()
