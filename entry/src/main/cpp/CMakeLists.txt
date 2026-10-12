cmake_minimum_required(VERSION 3.16)
project(hmrdp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT OHOS_ARCH)
    set(OHOS_ARCH arm64-v8a)
endif()

# The module's own native code runs at -O2 in *both* build modes. The OHOS
# toolchain's debug default (-O0 -g -fno-limit-debug-info, see ohos.toolchain.cmake)
# produces frames and variables that do not reflect what actually executes, so it
# has no debugging value for this codebase, and it ships noticeably more code.
# Overriding the flags rather than CMAKE_BUILD_TYPE is what survives hvigor's own
# -DCMAKE_BUILD_TYPE=Debug. -g is kept so a pre-strip stack is still symbolicable;
# the module strips every shipped library, so it costs nothing in the HAP. NDEBUG
# is deliberately absent in debug, so assert() still fires there.
set(CMAKE_C_FLAGS_DEBUG "-O2 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")

# FreeRDP/winpr headers bundled with the module, and the directory holding every
# shared library the module packages: FreeRDP, OpenSSL, zlib, opus and Moonlight.
# All of them are *shared* libraries produced by native/scripts/build-native.ps1
# (generated, gitignored), so the app links them by full path and they end up in
# DT_NEEDED instead of being folded into a copy per consumer - the reason
# libcrypto/libssl used to exist three times (see doc_agent/native-build.md).
set(FREERDP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/freerdp")
set(HMRDP_PREBUILT_LIBS "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/${OHOS_ARCH}")

# Moonlight (GameStream) client library: libmoonlight-common-c.so is a build
# product of native/scripts/build-native.ps1 (enet/nanors are folded into it) and
# therefore a *required* input of the app build - it is our own feature, not an
# optional platform capability. A missing library means the native build was not
# run, so fail the configure step with a clear instruction instead of silently
# building a binary without the feature (the same way a missing FreeRDP library
# fails the link).
set(MOONLIGHT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/moonlight")

# Opus: the Moonlight audio path decodes the host's Opus multistream packets with
# this library (moonlight-common-c only frames Opus, it does not decode). Same
# rule as the other dependencies: shared, shipped from entry/libs/<abi>/, and a
# required input - the app does not build a Moonlight without audio.
set(OPUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/opus")

# Every library in this list is both packaged and linked: the list is what
# "the native build ran" means for this module, and keeping it explicit avoids
# depending on the linker following DT_NEEDED transitively to resolve
# libmoonlight-common-c.so's own libcrypto reference.
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

# core/ holds the native resource drivers and hot paths (FreeRDP host, gdi
# frame pipeline, GPU presenters, video/audio, Moonlight transport); binding/
# holds the thin Node-API translation layer. Both are on the include path so the
# headers keep their bare names.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/binding
    ${FREERDP_ROOT}/include/freerdp3
    ${FREERDP_ROOT}/include/winpr3
    ${OPUS_ROOT}/include
)

# --- SPIR-V shaders -----------------------------------------------------------
# glslang_validator ships inside the OHOS SDK toolchains, one level up from the
# NDK root the toolchain file points OHOS_SDK_NATIVE at, so no external Vulkan SDK
# is needed. Each shader is compiled to SPIR-V and embedded in libhmrdp.so as a
# word array, which makes "the SPIR-V ships with the HAP" automatic.
# This block must precede add_library(): HMRDP_SHADER_HEADERS is expanded there.
set(HMRDP_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(NOT HMRDP_GLSLANG)
    set(HMRDP_GLSLANG "${OHOS_SDK_NATIVE}/../toolchains/glslang_validator.exe")
endif()

# <source file>|<C++ symbol>, e.g. "present_quad.vert|PresentQuadVert". Each entry
# yields a `k<Symbol>Spv[]` word array in <source file>.h. The presenter quad does
# the channel swap + letterbox on the GPU - see hmrdp_vk_renderer.cpp.
set(HMRDP_SHADERS
  "present_quad.vert|PresentQuadVert"
  "present_quad.frag|PresentQuadFrag"
)

# FSR 1.0 (the alternative 超分 backend, MIT): its two passes #include the vendored
# AMD headers, so they only build once native/scripts/fetch-sources.ps1 has run.
# Without the tree the presenter simply has no FSR backend to offer, the same way a
# missing HMS sysroot leaves out XEngine.
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

# Generated SPIR-V headers (one <source file>.h per entry in HMRDP_SHADERS) live
# outside the source tree, so include that directory.
target_include_directories(hmrdp PRIVATE "${HMRDP_SHADER_OUT}")

# Dead-strip at link time: the compile side already emits -ffunction-sections /
# -fdata-sections (OHOS toolchain default), so code and data libhmrdp does not
# reach is dropped instead of shipped. .init_array - the Node-API constructor that
# makes the module loadable - is KEEP-ed by the default linker script.
target_link_options(hmrdp PRIVATE "LINKER:--gc-sections")

# 超分 needs XEngine's headers, which ship in the HMS sysroot (sibling of the
# openharmony one the toolchain points at) - not in the plain NDK sysroot. The
# library itself is still dlopen()ed (hmrdp_xeg.cpp), so nothing links against it
# and a build without the HMS SDK degrades to "no 超分" instead of failing.
set(HMS_SYSROOT "${OHOS_SDK_NATIVE}/../../hms/native/sysroot")
if(EXISTS "${HMS_SYSROOT}/usr/include/xengine/xeg_vulkan_spatial_upscale.h")
    target_include_directories(hmrdp PRIVATE "${HMS_SYSROOT}/usr/include")
    # application_target_sdk_version.h (pulled in by the XEngine headers) lives in
    # the openharmony sysroot, which is already on the include path.
    target_compile_definitions(hmrdp PRIVATE HMRDP_HAVE_XENGINE=1)
    message(STATUS "HmRdp: XEngine (超分) headers at ${HMS_SYSROOT}")
else()
    message(WARNING "HmRdp: XEngine headers not found under ${HMS_SYSROOT}; 超分 disabled")
endif()

# FreeRDP is built without library versioning (see native/scripts/build-native.ps1
# + -DWITH_LIBRARY_VERSIONING=OFF), so each library is a single version-less
# libX.so whose SONAME is also libX.so. Linking it records libX.so in DT_NEEDED,
# matching the packaged files exactly (no libX.so.3 duplicate). The OpenSSL
# (libcrypto.so / libssl.so), opus and Moonlight libraries follow the same rule.
#
# libvulkan.so is deliberately NOT linked: it is dlopen()ed at runtime (see
# hmrdp_vk_context.h) so a device without a usable Vulkan driver degrades to the
# FreeRDP gdi path instead of failing to load the application.
#
# `native_window` turns the XComponent surface id into the OHNativeWindow the
# presenters hand to Vulkan/EGL. EGL/GLESv3 back the GLES presenter, which is the
# fallback for devices whose Vulkan cannot present (hmrdp_presenter.cpp).
#
# `ffrt.z` is the platform task queue (hmrdp_parallel.cpp). The patched decoder
# picks it up through weak symbols, so a build without it falls back to the
# codec's own WinPR pool instead of failing.
#
# `ohinput` backs the 快捷键穿透 key event interceptor
# (OH_Input_AddKeyEventInterceptor, needs the INTERCEPT_INPUT_EVENT ACL).
# `native_media_vdec` / `native_media_core` / `native_media_codecbase` are the
# AVCodec NDK the Moonlight video decoder uses (device-required media stack).
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
    # HUKS wraps the Moonlight client private key (AES-256-GCM under a
    # non-exportable key); it is a core security service present on every device.
    libhuks_ndk.z.so
    libohinput.so
    EGL
    GLESv3
    # FreeRDP + OpenSSL + zlib + Moonlight, all shared and all shipped from
    # entry/libs/<abi>/ (HMRDP_PREBUILT_LIB_PATHS above). OpenSSL appears exactly
    # once, so there is no second copy for a symbol to bind to.
    ${HMRDP_PREBUILT_LIB_PATHS}
)

target_include_directories(hmrdp PRIVATE "${MOONLIGHT_ROOT}/include")
message(STATUS "HmRdp: third-party shared libraries linked from ${HMRDP_PREBUILT_LIBS}")

# -Wno-unused-command-line-argument silences the OHOS toolchain passing
# --gcc-toolchain to compile-only invocations (clang reports it as unused).
# The optimisation level is pinned at the top of this file (-O2 in every mode).
target_compile_options(hmrdp PRIVATE -Wall -Wextra -fPIC
    -Wno-unused-command-line-argument)
target_compile_definitions(hmrdp PRIVATE NAPI_VERSION=8 VK_USE_PLATFORM_OHOS=1)
if(HMRDP_HAVE_FSR)
    target_compile_definitions(hmrdp PRIVATE HMRDP_HAVE_FSR=1)
endif()
