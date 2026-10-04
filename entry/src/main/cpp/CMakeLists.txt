cmake_minimum_required(VERSION 3.16)
project(hmrdp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT OHOS_ARCH)
    set(OHOS_ARCH arm64-v8a)
endif()

# FreeRDP headers bundled with the module, and the shared libraries that are
# packaged into entry/libs/<abi>.
set(FREERDP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/freerdp")
set(FREERDP_LIBS "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/${OHOS_ARCH}")

# libhmrdp links its own OpenSSL static archive, needed only by
# moonlight-common-c's control-stream crypto (src/PlatformCrypto.c: AES/GCM +
# RAND). native/scripts/build-native.ps1 syncs libcrypto.a under
# thirdparty/openssl/lib/<abi>/ (generated, gitignored). It is linked before
# FreeRDP's shared libraries and kept local with --exclude-libs, so
# moonlight-common-c's EVP_* bind to this copy instead of the OpenSSL FreeRDP
# statically links into libwinpr3.so (the reason Android links its own libcrypto
# with -Wl,--exclude-libs,ALL). Absent (no native build) means the Moonlight
# client is not linked either, so the build still succeeds - the same degrade
# path FSR and XEngine use. See doc_agent/native-libraries.md.
set(OPENSSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openssl")
set(HMRDP_OPENSSL_ARCHIVES "")
if(EXISTS "${OPENSSL_ROOT}/lib/${OHOS_ARCH}/libcrypto.a")
    set(HMRDP_OPENSSL_ARCHIVES "${OPENSSL_ROOT}/lib/${OHOS_ARCH}/libcrypto.a")
endif()

# Moonlight (GameStream) client library. Unlike FreeRDP it ships as *static*
# archives (native/scripts/build-native.ps1 collects them into
# thirdparty/moonlight/, which is generated and gitignored). Its OpenSSL
# references resolve against libhmrdp's own archive above, not winpr3.
set(MOONLIGHT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/moonlight")
set(HMRDP_HAVE_MOONLIGHT OFF)
if(EXISTS "${MOONLIGHT_ROOT}/include/Limelight.h" AND EXISTS "${MOONLIGHT_ROOT}/lib/${OHOS_ARCH}/libmoonlight-common-c.a")
    set(HMRDP_HAVE_MOONLIGHT ON)
endif()

set(HMRDP_MOONLIGHT_ARCHIVES "")
if(HMRDP_HAVE_MOONLIGHT)
    set(HMRDP_MOONLIGHT_ARCHIVES
        "${MOONLIGHT_ROOT}/lib/${OHOS_ARCH}/libmoonlight-common-c.a"
        "${MOONLIGHT_ROOT}/lib/${OHOS_ARCH}/libenet.a")
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
    core/hmrdp_file_clipboard.cpp
    core/hmrdp_moonlight.cpp
core/hmrdp_moonlight_session.cpp
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

# Static archives go *before* FreeRDP's shared libraries on the link line: the
# linker scans left to right, so moonlight-common-c's EVP_* references bind to
# libhmrdp's own OpenSSL (libcrypto.a) instead of the OpenSSL exported by
# libwinpr3.so linked further below. --exclude-libs keeps the archive symbols
# local so the two OpenSSL copies cannot interpose each other.
if(HMRDP_MOONLIGHT_ARCHIVES)
    target_link_libraries(hmrdp ${HMRDP_MOONLIGHT_ARCHIVES})
endif()
if(HMRDP_OPENSSL_ARCHIVES)
    target_link_libraries(hmrdp ${HMRDP_OPENSSL_ARCHIVES})
    target_link_options(hmrdp PRIVATE "LINKER:--exclude-libs,libcrypto.a")
endif()

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
# matching the packaged files exactly (no libX.so.3 duplicate).
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
    native_media_vdec
    native_media_core
    native_media_codecbase
    # HUKS wraps the Moonlight client private key (AES-256-GCM under a
    # non-exportable key); it is a core security service present on every device.
    libhuks_ndk.z.so
    libohinput.so
    EGL
    GLESv3
    ${FREERDP_LIBS}/libfreerdp3.so
    ${FREERDP_LIBS}/libwinpr3.so
    ${FREERDP_LIBS}/libfreerdp-client3.so
)

if(HMRDP_HAVE_MOONLIGHT)
    target_include_directories(hmrdp PRIVATE "${MOONLIGHT_ROOT}/include")
    target_compile_definitions(hmrdp PRIVATE HMRDP_HAVE_MOONLIGHT=1)
    message(STATUS "HmRdp: Moonlight client linked from ${MOONLIGHT_ROOT}/lib/${OHOS_ARCH}")
else()
    message(WARNING "HmRdp: Moonlight archives not found under ${MOONLIGHT_ROOT}; run native/scripts/build-native.ps1 to enable Moonlight")
endif()

# -Wno-unused-command-line-argument silences the OHOS toolchain passing
# --gcc-toolchain to compile-only invocations (clang reports it as unused).
# Do NOT hard-code an optimisation level here: the OHOS toolchain already picks
# flags from CMAKE_BUILD_TYPE (debug -> -O0 -g, release -> -O2 -DNDEBUG, see
# ohos.toolchain.cmake). A hard-coded -O2 would override the debug -O0 and make
# native debugging harder.
target_compile_options(hmrdp PRIVATE -Wall -Wextra -fPIC
    -Wno-unused-command-line-argument)
target_compile_definitions(hmrdp PRIVATE NAPI_VERSION=8 VK_USE_PLATFORM_OHOS=1)
if(HMRDP_HAVE_FSR)
    target_compile_definitions(hmrdp PRIVATE HMRDP_HAVE_FSR=1)
endif()
