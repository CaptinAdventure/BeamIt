cmake_minimum_required(VERSION 4.3)
# Static CRT: the host and input helper run on machines without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(BeamIt)
enable_language(RC)

set(CMAKE_CXX_STANDARD 26)

include(CTest)

include(cmake/CPM.cmake)
include(cmake/NvidiaNvenc.cmake)

# Embed the test web client as byte arrays: MSVC limits a single string literal to about 16 KB (C2026).
function(beamit_embed_bytes path out_var)
    file(READ "${path}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    string(REGEX REPLACE "((0x..,){32})" "\\1\n    " hex "${hex}")
    set(${out_var} "${hex}" PARENT_SCOPE)
endfunction()
beamit_embed_bytes("${CMAKE_CURRENT_SOURCE_DIR}/HTML/index.html" BEAMIT_HTML_INDEX)
beamit_embed_bytes("${CMAKE_CURRENT_SOURCE_DIR}/HTML/beamit.js" BEAMIT_HTML_LIBRARY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/HTML/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/HTML/beamit.js"
)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(src/host/HtmlAssets.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/HtmlAssets.hpp" @ONLY)

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
    NAME opus
    GIT_TAG v1.5.2
    GITHUB_REPOSITORY xiph/opus
    OPTIONS
        "BUILD_TESTING OFF"
        "OPUS_BUILD_TESTING OFF"
        "OPUS_BUILD_PROGRAMS OFF"
        "OPUS_INSTALL_PKG_CONFIG_MODULE OFF"
        "OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF"
        "OPUS_STATIC_RUNTIME ON"
)

include(cmake/BeamItDependencies.cmake)
beamit_add_network_dependencies()

set(BEAMIT_HOST_SOURCES
    src/host/AudioCapture.cpp
    src/host/WasapiAudioCapture.cpp
    src/media/OpusAudio.cpp
    src/host/AudioEndpoint.cpp
    src/host/wss/AudioSocket.cpp
    src/host/wss/ControlAudio.cpp
    src/host/AttentionMonitor.cpp
    src/host/Clipboard.cpp
    src/host/HostServer.cpp
    src/host/InputInjector.cpp
    src/host/InputProtocol.cpp
    src/host/ElevatedInput.cpp
    src/host/StreamPipeline.cpp
    src/host/TlsIdentity.cpp
    src/host/WindowIcon.cpp
    src/host/WindowTracker.cpp
    src/host/wss/ControlDesktop.cpp
    src/host/wss/ControlInput.cpp
    src/host/wss/ControlSession.cpp
    src/host/wss/ControlStats.cpp
    src/host/wss/Fields.cpp
    src/host/wss/HttpFrontend.cpp
    src/host/wss/StreamSocket.cpp
    src/media/BitstreamInfo.cpp
    src/net/Crypto.cpp
)

add_executable(BeamIt
    src/main.cpp
    src/host/TrayHost.cpp
    src/host/TrayHost.rc
    src/ApplicationWindows.cpp
    src/capture/DxgiCapturer.cpp
    src/capture/WgcCapturer.cpp
    src/media/D3D12VideoEncoder.cpp
    src/media/AutoVideoEncoder.cpp
    src/media/CodecHeaders.cpp
    ${BEAMIT_HOST_SOURCES}
)
target_include_directories(BeamIt PRIVATE include src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(BeamIt PRIVATE opus spdlog CLI11::CLI11 Boost::beast Boost::json beamit_wintls
    d3d11 d3d12 dxgi windowsapp ws2_32 user32 dwmapi shcore ole32 windowscodecs shell32 avrt winmm advapi32 mmdevapi)
target_compile_definitions(BeamIt PUBLIC NOMINMAX _WIN32_WINNT=0x0A00)

# Separate privilege boundary: no network, credentials, capture, clipboard or encoder dependencies.
add_executable(BeamItInputHelper WIN32
    src/InputHelperMain.cpp
    src/host/InputInjector.cpp
    src/host/InputProtocol.cpp
    src/host/ElevatedInput.cpp
)
target_include_directories(BeamItInputHelper PRIVATE src)
target_compile_definitions(BeamItInputHelper PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(BeamItInputHelper PRIVATE spdlog user32 dwmapi advapi32 shell32 ole32 bcrypt avrt winmm)
add_dependencies(BeamIt BeamItInputHelper)
set_property(TARGET BeamIt APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Install-BeamItCert.ps1")
add_custom_command(TARGET BeamIt POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/Install-BeamItCert.ps1" "$<TARGET_FILE_DIR:BeamIt>/Install-BeamItCert.ps1"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${opus_SOURCE_DIR}/COPYING" "$<TARGET_FILE_DIR:BeamIt>/Opus-LICENSE.txt"
)
if(TARGET NVIDIA::NVENCHeaders)
    target_sources(BeamIt PRIVATE src/media/NvencVideoEncoder.cpp)
    target_link_libraries(BeamIt PRIVATE NVIDIA::NVENCHeaders)
    target_compile_definitions(BeamIt PRIVATE BEAMIT_HAS_NVENC=1)
endif()

if(BUILD_TESTING)
    add_executable(BeamItAudioTestSource WIN32 tests/AudioTestSource.cpp)
    target_compile_definitions(BeamItAudioTestSource PRIVATE NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(BeamItAudioTestSource PRIVATE ole32 mmdevapi shell32)
    beamit_add_catch2()

    add_executable(BeamItTests
        tests/AudioCaptureTests.cpp
        tests/AudioTests.cpp
        tests/AudioWssTests.cpp
        tests/CaptureTypesTests.cpp
        tests/CodecHeadersTests.cpp
        tests/BeastTests.cpp
        tests/ProtocolTests.cpp
        tests/InputTests.cpp
        tests/InputIpcTests.cpp
        tests/AttentionTests.cpp
        tests/ClipboardTests.cpp
        tests/WindowTrackerTests.cpp
        tests/WindowIconTests.cpp
        tests/WssFieldsTests.cpp
        tests/HostControlTests.cpp
        src/ApplicationWindows.cpp
        ${BEAMIT_HOST_SOURCES}
        src/capture/DxgiCapturer.cpp
        src/capture/WgcCapturer.cpp
        src/media/D3D12VideoEncoder.cpp
        src/media/AutoVideoEncoder.cpp
        src/media/CodecHeaders.cpp
    )
    target_include_directories(BeamItTests PRIVATE include src "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_dependencies(BeamItTests BeamItInputHelper)
    add_dependencies(BeamItTests BeamItAudioTestSource)
    target_compile_definitions(BeamItTests PRIVATE BEAMIT_USE_CATCH2=1 NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(BeamItTests PRIVATE opus Catch2::Catch2WithMain spdlog Boost::beast Boost::json beamit_wintls
        d3d11 d3d12 dxgi windowsapp ws2_32 user32 dwmapi shcore ole32 windowscodecs shell32 avrt winmm advapi32 mmdevapi)
    if(TARGET NVIDIA::NVENCHeaders)
        target_sources(BeamItTests PRIVATE tests/NvencVideoEncoderTests.cpp src/media/NvencVideoEncoder.cpp)
        target_link_libraries(BeamItTests PRIVATE NVIDIA::NVENCHeaders)
        target_compile_definitions(BeamItTests PRIVATE BEAMIT_HAS_NVENC=1)
    endif()
    add_test(NAME BeamItTests COMMAND BeamItTests)
endif()
