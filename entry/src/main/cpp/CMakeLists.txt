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

# OpenSSL headers, needed to compile the Moonlight HTTP/pairing layer (TLS,
# RSA/AES/SHA, self-signed client certificate generation) and referenced by
# moonlight-common-c's pairing AES. Only the headers are synced under
# thirdparty/openssl/ by native/scripts/sync-openssl.ps1 (generated, gitignored):
# the OpenSSL *code* is the copy FreeRDP already statically links into
# libwinpr3.so and exports, so libhmrdp resolves SSL_* / EVP_* / X509_* against
# winpr3 instead of carrying a duplicate OpenSSL in the process. See
# doc_agent/native-libraries.md.
#
# Headers absent means the Moonlight HTTP layer compiles to "unsupported" and
# nothing links, so a checkout without the native build still builds - the same
# degrade path FSR and XEngine use.
set(OPENSSL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/openssl")
set(HMRDP_HAVE_OPENSSL_HEADERS OFF)
if(EXISTS "${OPENSSL_ROOT}/include/openssl/ssl.h")
    set(HMRDP_HAVE_OPENSSL_HEADERS ON)
endif()

# Moonlight (GameStream) client library. Unlike FreeRDP it ships as *static*
# archives (native/scripts/build-moonlight.ps1 collects them into
# thirdparty/moonlight/, which is generated and gitignored). Its OpenSSL
# references resolve against the same winpr3 OpenSSL as above.
set(MOONLIGHT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/moonlight")
set(HMRDP_HAVE_MOONLIGHT OFF)
if(EXISTS "${MOONLIGHT_ROOT}/include/Limelight.h" AND EXISTS "${MOONLIGHT_ROOT}/lib/${OHOS_ARCH}/libmoonlight-common-c.a")
    set(HMRDP_HAVE_MOONLIGHT ON)
endif()

# The Moonlight HTTP/pairing layer only needs the OpenSSL headers (no
# moonlight-common-c), so it is enabled on them alone. This is what lets
# serverinfo / pairing / the app list work before a stream is ever established.
set(HMRDP_HAVE_MOONLIGHT_HTTP ${HMRDP_HAVE_OPENSSL_HEADERS})

# engine/ holds the native resource drivers and hot paths (FreeRDP host, gdi
# frame pipeline, GPU presenters, video/audio, Moonlight transport); binding/
# holds the thin Node-API translation layer. Both are on the include path so the
# headers keep their bare names.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
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
    engine/hmrdp_session.cpp
    engine/hmrdp_file_clipboard.cpp
    engine/hmrdp_moonlight.cpp
    engine/hmrdp_moonlight_http.cpp
    engine/hmrdp_moonlight_cred.cpp
    engine/hmrdp_moonlight_pair.cpp
    engine/hmrdp_video_decoder.cpp
    engine/hmrdp_audio.cpp
    engine/hmrdp_gfx_capture.cpp
    engine/hmrdp_gfx_cpu.cpp
    engine/hmrdp_gfx_driver.cpp
    engine/hmrdp_gfx_work.cpp
    engine/hmrdp_decode_tuning.cpp
    engine/hmrdp_parallel.cpp
    engine/hmrdp_replay.cpp
    engine/hmrdp_frame_pipeline.cpp
    engine/hmrdp_presenter.cpp
    engine/hmrdp_gles_presenter.cpp
    engine/hmrdp_vk_context.cpp
    engine/hmrdp_vk_renderer.cpp
    engine/hmrdp_xeg.cpp
    ${HMRDP_SHADER_HEADERS}
)

# Generated SPIR-V headers (one <source file>.h per entry in HMRDP_SHADERS) live
# outside the source tree, so include that directory.
target_include_directories(hmrdp PRIVATE "${HMRDP_SHADER_OUT}")

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

# FreeRDP is built without library versioning (see native/scripts/build-freerdp.ps1
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

# Moonlight static archives (built by native/scripts/build-moonlight.ps1) plus the
# OpenSSL headers the HTTP/pairing layer compiles against. OpenSSL *code* is not
# linked here: it comes from libwinpr3.so above, which already statically carries
# and exports it (see the OPENSSL_ROOT note near the top). Plain signature (no
# PRIVATE): the FreeRDP link above uses plain, and CMake rejects mixing the two on
# one target.
if(HMRDP_HAVE_OPENSSL_HEADERS)
    target_include_directories(hmrdp PRIVATE "${OPENSSL_ROOT}/include")
endif()

if(HMRDP_HAVE_MOONLIGHT)
    target_include_directories(hmrdp PRIVATE "${MOONLIGHT_ROOT}/include")
    target_compile_definitions(hmrdp PRIVATE HMRDP_HAVE_MOONLIGHT=1)
    target_link_libraries(hmrdp
        "${MOONLIGHT_ROOT}/lib/${OHOS_ARCH}/libmoonlight-common-c.a"
        "${MOONLIGHT_ROOT}/lib/${OHOS_ARCH}/libenet.a"
    )
    message(STATUS "HmRdp: Moonlight client linked from ${MOONLIGHT_ROOT}/lib/${OHOS_ARCH}")
else()
    message(WARNING "HmRdp: Moonlight archives not found under ${MOONLIGHT_ROOT}; run native/scripts/build-moonlight.ps1 to enable Moonlight")
endif()

if(HMRDP_HAVE_MOONLIGHT_HTTP)
    target_compile_definitions(hmrdp PRIVATE HMRDP_HAVE_MOONLIGHT_HTTP=1)
    message(STATUS "HmRdp: Moonlight HTTP/pairing layer enabled (OpenSSL headers at ${OPENSSL_ROOT})")
else()
    message(WARNING "HmRdp: OpenSSL headers not found under ${OPENSSL_ROOT}; run native/scripts/sync-openssl.ps1 to enable the Moonlight HTTP layer")
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
