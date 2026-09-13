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

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FREERDP_ROOT}/include/freerdp3
    ${FREERDP_ROOT}/include/winpr3
)

add_library(hmrdp SHARED
    hmrdp_napi.cpp
    hmrdp_session.cpp
    hmrdp_renderer.cpp
    hmrdp_egl.cpp
    hmrdp_audio.cpp
    hmrdp_rfx.cpp
    hmrdp_gfx_capture.cpp
    hmrdp_replay.cpp
)

# FreeRDP is built without library versioning (see native/scripts/build-freerdp.ps1
# + -DWITH_LIBRARY_VERSIONING=OFF), so each library is a single version-less
# libX.so whose SONAME is also libX.so. Linking it records libX.so in DT_NEEDED,
# matching the packaged files exactly (no libX.so.3 duplicate).
target_link_libraries(hmrdp
    ace_napi.z
    hilog_ndk.z
    native_window
    EGL
    GLESv3
    ${FREERDP_LIBS}/libfreerdp3.so
    ${FREERDP_LIBS}/libwinpr3.so
    ${FREERDP_LIBS}/libfreerdp-client3.so
)

# -Wno-unused-command-line-argument silences the OHOS toolchain passing
# --gcc-toolchain to compile-only invocations (clang reports it as unused).
# Do NOT hard-code an optimisation level here: the OHOS toolchain already picks
# flags from CMAKE_BUILD_TYPE (debug -> -O0 -g, release -> -O2 -DNDEBUG, see
# ohos.toolchain.cmake). A hard-coded -O2 would override the debug -O0 and make
# native debugging harder.
target_compile_options(hmrdp PRIVATE -Wall -Wextra -fPIC
    -Wno-unused-command-line-argument)
target_compile_definitions(hmrdp PRIVATE NAPI_VERSION=8)
