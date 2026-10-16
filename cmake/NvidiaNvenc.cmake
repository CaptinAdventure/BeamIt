include(FetchContent)

option(BEAMIT_FETCH_NVENC_INTERFACE "Populate pinned NVIDIA Video Codec SDK headers from the local archive" ON)

if(BEAMIT_FETCH_NVENC_INTERFACE)
    set(beamit_nvenc_archive "${PROJECT_SOURCE_DIR}/vendor/Video_Codec_SDK_13.0.37.zip")
    if(NOT EXISTS "${beamit_nvenc_archive}")
        message(FATAL_ERROR
            "Place Video_Codec_SDK_13.0.37.zip in vendor/ or disable BEAMIT_FETCH_NVENC_INTERFACE")
    endif()
    FetchContent_Declare(
        beamit_nvenc_interface
        URL "${beamit_nvenc_archive}"
        URL_HASH SHA256=828c5382a9999191eebd26fc5e3c62dc545aeccd95b6e307d9f7de5103889954
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/vendor/nvidia_nvend"
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    )
    FetchContent_MakeAvailable(beamit_nvenc_interface)

    if(NOT EXISTS "${beamit_nvenc_interface_SOURCE_DIR}/Interface/nvEncodeAPI.h")
        message(FATAL_ERROR "NVIDIA Video Codec Interface archive is missing Interface/nvEncodeAPI.h")
    endif()

    add_library(beamit_nvenc_headers INTERFACE)
    add_library(NVIDIA::NVENCHeaders ALIAS beamit_nvenc_headers)
    target_include_directories(beamit_nvenc_headers SYSTEM INTERFACE
        "${beamit_nvenc_interface_SOURCE_DIR}/Interface"
    )
endif()
