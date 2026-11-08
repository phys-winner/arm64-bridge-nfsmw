cmake_minimum_required(VERSION 3.22)
project(arm32_bridge LANGUAGES C CXX)

# Pass -DBRIDGE_TARGET=<name> to match the game's System.loadLibrary() call.
set(BRIDGE_TARGET arm32_bridge CACHE STRING "Bridge library target name")
option(ARM32_BRIDGE_ENABLE_LOGGING "Enable informational bridge diagnostics" OFF)
option(BRIDGE_BUILD_TESTS "Build standalone Android bridge regression executable" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Size: applied before FetchContent so dynarmic/fmt/mcl inherit them. Only
# JNI_OnLoad has to be exported (everything else is registered at runtime), so
# hiding the rest drops the dynsym table and most of .rela.
add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden
                    -ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections -Wl,--icf=all -Wl,--exclude-libs,ALL)

# Boost is supplied externally; BOOST_ROOT points to an extracted Boost release.
set(Boost_NO_BOOST_CMAKE ON)
find_path(Boost_INCLUDE_DIR boost/version.hpp HINTS "${BOOST_ROOT}" "$ENV{BOOST_ROOT}"
    PATH_SUFFIXES include NO_CMAKE_FIND_ROOT_PATH)
find_package(Boost 1.83 REQUIRED)
include_directories(SYSTEM ${Boost_INCLUDE_DIRS})

# Configure Dynarmic.
# NOTE: the real option is DYNARMIC_FRONTENDS (a list), not DYNARMIC_ENABLE_A32/A64 --
# the old names were silently ignored, so A64 was being built too.
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)  # cross-compiling: never pick up host libs

message(STATUS "Configuring pinned Dynarmic dependency...")
FetchContent_Declare(
    dynarmic
    GIT_REPOSITORY https://github.com/azahar-emu/dynarmic.git
    GIT_TAG        e77b1ba0b7da7cbe93021b01a663acfe7c4dd516
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DDYNARMIC_SOURCE_DIR=<SOURCE_DIR>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply.cmake
)
FetchContent_MakeAvailable(dynarmic)

set(BRIDGE_SOURCES
    src/elf_loader.cpp
    src/dynarmic_host.cpp
    src/guest_cp15.cpp
    src/thunk_manager.cpp
    src/thunks_gl.cpp
    src/thunks_gl_gles2_core.cpp
    src/thunks_libc.cpp
    src/thunks_posix.cpp
    src/thunks_libc_extra.cpp
    src/thunks_zlib.cpp
    src/thunks_android.cpp
    src/thunks_opensles.cpp   # drop this line if the game is not on FMOD Ex
    src/jni_fake_env.cpp
    src/framerate.cpp
    src/options.cpp
    src/graphics.cpp
    src/portstrings.cpp
    src/log_gate.cpp
    src/prof.cpp
    src/jni_bridge.cpp
)

# Logcat gate (src/log_gate.cpp, stage 12): liblog's print/write/vprint calls of every object linked into a target
# go through the gate, which drops them below ANDROID_LOG_FATAL unless debug.nfsbridge.logs is 1.
set(BRIDGE_LOG_WRAP
    "-Wl,--wrap=__android_log_print"
    "-Wl,--wrap=__android_log_write"
    "-Wl,--wrap=__android_log_vprint"
)

add_library(${BRIDGE_TARGET} SHARED ${BRIDGE_SOURCES})

target_include_directories(${BRIDGE_TARGET} PRIVATE
    src
    ${dynarmic_SOURCE_DIR}/include
)

target_link_libraries(${BRIDGE_TARGET} PRIVATE
    dynarmic
    GLESv3
    EGL
    OpenSLES
    log
    android
    jnigraphics
    dl
    m
    z
)

target_compile_definitions(${BRIDGE_TARGET} PRIVATE
    ANDROID
    __ANDROID__
    ARM32_BRIDGE_ENABLE_LOGGING=$<BOOL:${ARM32_BRIDGE_ENABLE_LOGGING}>
)

# Android 15+ devices boot with 16 KB pages; a 4 KB-aligned LOAD segment trips
# the "app isn't 16 KB compatible" loader warning.
target_link_options(${BRIDGE_TARGET} PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
    "-Wl,-z,relro"
    "-Wl,-z,now"
    ${BRIDGE_LOG_WRAP}
)

if(BRIDGE_BUILD_TESTS)
    set(JNI_TEST_SOURCES ${BRIDGE_SOURCES})
    list(REMOVE_ITEM JNI_TEST_SOURCES src/jni_bridge.cpp)
    add_library(bridge_jni_regression SHARED tests/jni_regression.cpp ${JNI_TEST_SOURCES})
    target_include_directories(bridge_jni_regression PRIVATE src ${dynarmic_SOURCE_DIR}/include)
    target_link_libraries(bridge_jni_regression PRIVATE dynarmic GLESv3 EGL OpenSLES log android jnigraphics dl m z)
    target_compile_definitions(bridge_jni_regression PRIVATE ARM32_BRIDGE_ENABLE_LOGGING=0)
    target_link_options(bridge_jni_regression PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384" ${BRIDGE_LOG_WRAP})
    add_executable(bridge_regression tests/bridge_regression.cpp ${BRIDGE_SOURCES})
    target_include_directories(bridge_regression PRIVATE src ${dynarmic_SOURCE_DIR}/include)
    target_link_libraries(bridge_regression PRIVATE dynarmic GLESv3 EGL OpenSLES log android jnigraphics dl m z)
    target_compile_definitions(bridge_regression PRIVATE ARM32_BRIDGE_ENABLE_LOGGING=0)
    target_link_options(bridge_regression PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384" ${BRIDGE_LOG_WRAP})
endif()
