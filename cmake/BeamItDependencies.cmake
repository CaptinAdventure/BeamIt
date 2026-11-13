# Checksum/tag-pinned dependencies shared by the host and the Windows client core.
# Include cmake/CPM.cmake first.

set(BEAMIT_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

macro(beamit_add_network_dependencies)
    CPMAddPackage(
        NAME Boost
        VERSION 1.90.0
        URL https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-cmake.tar.xz
        URL_HASH SHA256=aca59f889f0f32028ad88ba6764582b63c916ce5f77b31289ad19421a96c555f
        SYSTEM YES
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
        PATCH_COMMAND "${CMAKE_COMMAND}" -P "${BEAMIT_CMAKE_DIR}/patches/wintls-native-handle.cmake"
    )
    if(NOT TARGET beamit_wintls)
        add_library(beamit_wintls INTERFACE)
        # Beast/Asio coroutine translation units exceed the default COFF section limit.
        target_compile_options(beamit_wintls INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
        target_include_directories(beamit_wintls SYSTEM INTERFACE "${wintls_SOURCE_DIR}/include")
        target_link_libraries(beamit_wintls INTERFACE Boost::beast crypt32 secur32 bcrypt)
    endif()
endmacro()

macro(beamit_add_catch2)
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
endmacro()
