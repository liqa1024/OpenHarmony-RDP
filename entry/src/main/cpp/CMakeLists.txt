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
)

target_link_directories(hmrdp PRIVATE ${FREERDP_LIBS})

target_link_libraries(hmrdp
    ace_napi.z
    hilog_ndk.z
    native_window
    EGL
    GLESv3
    freerdp3
    winpr3
    freerdp-client3
)

# -Wno-unused-command-line-argument silences the OHOS toolchain passing
# --gcc-toolchain to compile-only invocations (clang reports it as unused).
target_compile_options(hmrdp PRIVATE -Wall -Wextra -O2 -fPIC
    -Wno-unused-command-line-argument)
target_compile_definitions(hmrdp PRIVATE NAPI_VERSION=8)
