cmake_minimum_required(VERSION 3.16)
project(hmrdp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT OHOS_ARCH)
    set(OHOS_ARCH arm64-v8a)
endif()

# The module's own code is -O2 in both build modes: the OHOS toolchain's -O0 debug default has no
# debugging value here. Overriding the flags (not CMAKE_BUILD_TYPE) survives hvigor; NDEBUG stays off in debug.
set(CMAKE_C_FLAGS_DEBUG "-O2 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")

# Bundled FreeRDP/winpr headers and the shared libraries the module packages (all produced by
# native/scripts/build-native.ps1): linked by full path so they land in DT_NEEDED.
set(FREERDP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/freerdp")
set(HMRDP_PREBUILT_LIBS "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/${OHOS_ARCH}")

# libmoonlight-common-c.so is a required input of the app build; a missing library fails the configure step.
set(MOONLIGHT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/moonlight")

# Opus decodes the host's Opus multistream packets for the Moonlight audio path; a required input like the others.
set(OPUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/opus")

# Every library here is both packaged and linked; keeping it explicit avoids relying on transitive DT_NEEDED resolution.
set(HMRDP_PREBUILT_LIB_NAMES
    libcrypto.so
    libssl.so
    libz.so
    libfreerdp3.so
    libwinpr3.so
    libfreerdp-client3.so
    libmoonlight-common-c.so
    libopus.so)
foreach(_lib IN LISTS HMRDP_PREBUILT_LIB_NAMES)
    if(NOT EXISTS "${HMRDP_PREBUILT_LIBS}/${_lib}")
        message(FATAL_ERROR
            "HmRdp: ${_lib} missing under ${HMRDP_PREBUILT_LIBS}; "
            "run native/scripts/build-native.ps1 -Arch ${OHOS_ARCH} first")
    endif()
    list(APPEND HMRDP_PREBUILT_LIB_PATHS "${HMRDP_PREBUILT_LIBS}/${_lib}")
endforeach()
if(NOT EXISTS "${MOONLIGHT_ROOT}/include/Limelight.h")
    message(FATAL_ERROR
        "HmRdp: Limelight.h missing under ${MOONLIGHT_ROOT}/include; "
        "run native/scripts/build-native.ps1 first")
endif()
if(NOT EXISTS "${OPUS_ROOT}/include/opus/opus_multistream.h")
    message(FATAL_ERROR
        "HmRdp: opus_multistream.h missing under ${OPUS_ROOT}/include; "
        "run native/scripts/build-native.ps1 first")
endif()

# core/ is the native drivers/hot paths, binding/ the Node-API layer; both on the include path so headers keep bare names.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/binding
    ${FREERDP_ROOT}/include/freerdp3
    ${FREERDP_ROOT}/include/winpr3
    ${OPUS_ROOT}/include
)

# glslang_validator ships in the OHOS SDK toolchains (no external Vulkan SDK); each shader is
# compiled to SPIR-V and embedded as a word array. Must precede add_library(): HMRDP_SHADER_HEADERS is used there.
set(HMRDP_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(NOT HMRDP_GLSLANG)
    set(HMRDP_GLSLANG "${OHOS_SDK_NATIVE}/../toolchains/glslang_validator.exe")
endif()

# "<source file>|<C++ symbol>": each entry yields a `k<Symbol>Spv[]` word array in <source file>.h.
set(HMRDP_SHADERS
  "present_quad.vert|PresentQuadVert"
  "present_quad.frag|PresentQuadFrag"
)

# FSR 1.0 (MIT) passes need the vendored AMD headers from fetch-sources.ps1; without them there is no FSR backend.
set(HMRDP_FSR_INCLUDE "")
set(HMRDP_HAVE_FSR OFF)
set(_fsr_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/third_party/fidelityfx-fsr-1.0.2/ffx-fsr")
if(EXISTS "${_fsr_root}/ffx_fsr1.h")
    set(HMRDP_FSR_INCLUDE "${_fsr_root}")
    set(HMRDP_HAVE_FSR ON)
    list(APPEND HMRDP_SHADERS
      "fsr_easu.frag|FsrEasuFrag"
      "fsr_rcas.frag|FsrRcasFrag"
    )
    message(STATUS "HmRdp: FSR 1.0 headers at ${_fsr_root}")
else()
    message(WARNING "HmRdp: FSR headers not found under ${_fsr_root}; run native/scripts/fetch-sources.ps1 to enable the FSR backend")
endif()

set(HMRDP_SHADER_HEADERS "")
foreach(_shader IN LISTS HMRDP_SHADERS)
    string(REPLACE "|" ";" _parts "${_shader}")
    list(GET _parts 0 _file)
    list(GET _parts 1 _symbol)
    set(_generated "${HMRDP_SHADER_OUT}/${_file}.h")
    add_custom_command(
        OUTPUT "${_generated}"
        COMMAND ${CMAKE_COMMAND}
                "-DHMRDP_GLSLANG=${HMRDP_GLSLANG}"
                "-DHMRDP_SRC=${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_file}"
                "-DHMRDP_OUT=${_generated}"
                "-DHMRDP_SYMBOL=${_symbol}"
                "-DHMRDP_INCLUDE=${HMRDP_FSR_INCLUDE}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_file}"
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
        COMMENT "SPIR-V ${_file}"
        VERBATIM)
    list(APPEND HMRDP_SHADER_HEADERS "${_generated}")
endforeach()

add_library(hmrdp SHARED
    binding/hmrdp_napi.cpp
    core/hmrdp_session.cpp
    core/hmrdp_bitmap.cpp
    core/hmrdp_file_clipboard.cpp
    core/hmrdp_moonlight.cpp
core/hmrdp_moonlight_session.cpp
    core/hmrdp_moonlight_audio.cpp
    core/hmrdp_oh_audio.cpp
    core/hmrdp_video_decoder.cpp
    core/hmrdp_audio.cpp
    core/hmrdp_gfx_capture.cpp
    core/hmrdp_gfx_cpu.cpp
    core/hmrdp_gfx_driver.cpp
    core/hmrdp_gfx_work.cpp
    core/hmrdp_decode_tuning.cpp
    core/hmrdp_parallel.cpp
    core/hmrdp_replay.cpp
    core/hmrdp_frame_pipeline.cpp
    core/hmrdp_presenter.cpp
    core/hmrdp_gles_presenter.cpp
    core/hmrdp_vk_context.cpp
    core/hmrdp_vk_renderer.cpp
    core/hmrdp_xeg.cpp
    ${HMRDP_SHADER_HEADERS}
)

# Generated SPIR-V headers live outside the source tree; add them to the include path.
target_include_directories(hmrdp PRIVATE "${HMRDP_SHADER_OUT}")

# --gc-sections drops unreached code/data (compile side is already -ffunction/-fdata-sections); .init_array is KEEP-ed.
target_link_options(hmrdp PRIVATE "LINKER:--gc-sections")

# 超分 needs XEngine headers from the HMS sysroot (not the plain NDK); the library is dlopen()ed, so a build without the HMS SDK degrades to "no 超分".
set(HMS_SYSROOT "${OHOS_SDK_NATIVE}/../../hms/native/sysroot")
if(EXISTS "${HMS_SYSROOT}/usr/include/xengine/xeg_vulkan_spatial_upscale.h")
    target_include_directories(hmrdp PRIVATE "${HMS_SYSROOT}/usr/include")
    # application_target_sdk_version.h (pulled by the XEngine headers) is in the openharmony sysroot already on the path.
    target_compile_definitions(hmrdp PRIVATE HMRDP_HAVE_XENGINE=1)
    message(STATUS "HmRdp: XEngine (超分) headers at ${HMS_SYSROOT}")
else()
    message(WARNING "HmRdp: XEngine headers not found under ${HMS_SYSROOT}; 超分 disabled")
endif()

# FreeRDP/OpenSSL/opus/Moonlight are version-less libX.so (SONAME libX.so), so DT_NEEDED matches the
# packaged files. libvulkan.so is dlopen()ed (degrade to gdi); ffrt.z is picked up weakly by the
# patched decoder (else the codec's WinPR pool); ohinput backs the key interceptor.
target_link_libraries(hmrdp
    ace_napi.z
    hilog_ndk.z
    ffrt.z
    native_window
    # OHAudio: the Moonlight low-latency playback path (FAST + audio workgroup).
    ohaudio
    native_media_vdec
    native_media_core
    native_media_codecbase
    # HUKS wraps the Moonlight client private key (AES-256-GCM under a non-exportable key).
    libhuks_ndk.z.so
    libohinput.so
    EGL
    GLESv3
    # All shared, shipped from entry/libs/<abi>/ (HMRDP_PREBUILT_LIB_PATHS); OpenSSL appears exactly once.
    ${HMRDP_PREBUILT_LIB_PATHS}
)

target_include_directories(hmrdp PRIVATE "${MOONLIGHT_ROOT}/include")
message(STATUS "HmRdp: third-party shared libraries linked from ${HMRDP_PREBUILT_LIBS}")

# -Wno-unused-command-line-argument silences the toolchain passing --gcc-toolchain to compile-only invocations.
target_compile_options(hmrdp PRIVATE -Wall -Wextra -fPIC
    -Wno-unused-command-line-argument)
target_compile_definitions(hmrdp PRIVATE NAPI_VERSION=8 VK_USE_PLATFORM_OHOS=1)
if(HMRDP_HAVE_FSR)
    target_compile_definitions(hmrdp PRIVATE HMRDP_HAVE_FSR=1)
endif()
