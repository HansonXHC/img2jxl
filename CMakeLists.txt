cmake_minimum_required(VERSION 3.21)
project(img2jxl VERSION 1.1.0 LANGUAGES C CXX)

option(IMG2JXL_GUI "Build the Qt GUI" ON)
option(IMG2JXL_TESTS "Build tests" ON)
option(IMG2JXL_FETCH_DEPS "Download missing dependencies online (last resort)" ON)

# Dependency resolution per library, in order:
#   1. vendored source in thirdparty/<lib>/  (works fully offline)
#   2. local override / prebuilt (see CACHE PATHs below)
#   3. online download via FetchContent (zlib.net / SourceForge / GitHub)
set(LIBPNG_SOURCE_DIR "" CACHE PATH "local libpng source tree")
set(LIBJPEG_PREBUILT_DIR "" CACHE PATH "prebuilt libjpeg-turbo (include/ + lib/)")
set(ZLIB_SOURCE_DIR "" CACHE PATH "local zlib source tree")
set(LIBJXL_SOURCE_DIR "" CACHE PATH "local libjxl source tree")

include(FetchContent)

# ---------------------------------------------------------------------------
# zlib (needed by libpng for PNG input decoding)
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zlib/CMakeLists.txt")
    message(STATUS "img2jxl: using vendored zlib")
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/zlib zlib-build)
    set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zlib" "${CMAKE_BINARY_DIR}/zlib-build")
    if(MSVC)
        set(ZLIB_LIBRARY "${CMAKE_BINARY_DIR}/zlib-build/zlibstatic.lib")
    elseif(APPLE OR UNIX)
        set(ZLIB_LIBRARY "${CMAKE_BINARY_DIR}/zlib-build/libz.a")   # OUTPUT_NAME z on unix
    else()
        set(ZLIB_LIBRARY "${CMAKE_BINARY_DIR}/zlib-build/libzlibstatic.a")
    endif()
elseif(ZLIB_SOURCE_DIR)
    message(STATUS "img2jxl: using local zlib at ${ZLIB_SOURCE_DIR}")
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory(${ZLIB_SOURCE_DIR} zlib-build)
    set(ZLIB_LIBRARY zlibstatic)
    set(ZLIB_INCLUDE_DIR "${ZLIB_SOURCE_DIR}")
else()
    find_package(ZLIB QUIET)
    if(NOT ZLIB_FOUND AND IMG2JXL_FETCH_DEPS)
        message(STATUS "img2jxl: fetching zlib")
        set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
        FetchContent_Declare(zlib
            URL https://zlib.net/fossils/zlib-1.3.1.tar.gz
                https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz)
        FetchContent_MakeAvailable(zlib)
        # satisfy libpng's find_package(ZLIB REQUIRED) with the fetched build:
        # preset what FindZLIB would find, with the real static-lib file
        set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
        if(MSVC)
            set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/zlibstatic.lib")
        elseif(APPLE OR UNIX)
            set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/libz.a")   # OUTPUT_NAME z on unix
        else()
            set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/libzlibstatic.a")
        endif()
    elseif(NOT ZLIB_FOUND)
        message(FATAL_ERROR "img2jxl: zlib not found; install it, vendor it under thirdparty/zlib, or enable IMG2JXL_FETCH_DEPS")
    endif()
endif()

# ---------------------------------------------------------------------------
# libpng 1.6.59 (PNG input decoding only; the APNG patch it carries is inert)
# ---------------------------------------------------------------------------
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(PNG_HARDWARE_OPTIMIZATIONS ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libpng/CMakeLists.txt")
    message(STATUS "img2jxl: using vendored libpng")
    add_subdirectory(thirdparty/libpng libpng-build)
elseif(LIBPNG_SOURCE_DIR)
    message(STATUS "img2jxl: using local libpng at ${LIBPNG_SOURCE_DIR}")
    add_subdirectory(${LIBPNG_SOURCE_DIR} libpng-build)
else()
    if(NOT IMG2JXL_FETCH_DEPS)
        message(FATAL_ERROR "img2jxl: no libpng; vendor it under thirdparty/libpng, pass -DLIBPNG_SOURCE_DIR, or enable IMG2JXL_FETCH_DEPS")
    endif()
    message(STATUS "img2jxl: fetching libpng v1.6.59")
    FetchContent_Declare(libpng
        URL https://downloads.sourceforge.net/project/libpng/libpng16/1.6.59/libpng-1.6.59.tar.gz
            https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.59.tar.gz)
    FetchContent_MakeAvailable(libpng)
endif()
if(TARGET zlibstatic)
    add_dependencies(png_static zlibstatic)
endif()
set(IMG2JXL_PNG_TARGET png_static)

# ---------------------------------------------------------------------------
# libjpeg-turbo 3.2.0 (SIMD off -> no NASM needed; pixel fallback path)
# ---------------------------------------------------------------------------
# Its bundled GNUInstallDirs computes blank install dirs when built as a
# subproject on Windows; preset sane values (we never install it anyway).
set(CMAKE_INSTALL_BINDIR "bin" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_DATAROOTDIR "share" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_DOCDIR "share/doc" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_INCLUDEDIR "include" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_LIBDIR "lib" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_MANDIR "share/man" CACHE PATH "" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libjpeg-turbo/CMakeLists.txt")
    message(STATUS "img2jxl: using vendored libjpeg-turbo")
    set(WITH_SIMD OFF CACHE BOOL "" FORCE)
    set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
    set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
    set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
    set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_JAVA OFF CACHE BOOL "" FORCE)
    set(WITH_FUZZ OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/libjpeg-turbo libjpeg-turbo-build)
    set(IMG2JXL_JPEG_TARGET jpeg-static)
elseif(LIBJPEG_PREBUILT_DIR)
    message(STATUS "img2jxl: using prebuilt libjpeg-turbo at ${LIBJPEG_PREBUILT_DIR}")
    if(NOT EXISTS "${LIBJPEG_PREBUILT_DIR}/lib/libjpeg.a")
        message(FATAL_ERROR "img2jxl: ${LIBJPEG_PREBUILT_DIR}/lib/libjpeg.a not found")
    endif()
    add_library(img2jxl_jpeg STATIC IMPORTED)
    set_target_properties(img2jxl_jpeg PROPERTIES
        IMPORTED_LOCATION "${LIBJPEG_PREBUILT_DIR}/lib/libjpeg.a"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBJPEG_PREBUILT_DIR}/include")
    set(IMG2JXL_JPEG_TARGET img2jxl_jpeg)
else()
    if(NOT IMG2JXL_FETCH_DEPS)
        message(FATAL_ERROR "img2jxl: no libjpeg; vendor it under thirdparty/libjpeg-turbo, pass -DLIBJPEG_PREBUILT_DIR, or enable IMG2JXL_FETCH_DEPS")
    endif()
    message(STATUS "img2jxl: fetching libjpeg-turbo 3.2.0")
    set(WITH_SIMD OFF CACHE BOOL "" FORCE)
    set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
    set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
    set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
    set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_JAVA OFF CACHE BOOL "" FORCE)
    set(WITH_FUZZ OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libjpeg-turbo
        URL https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz)
    FetchContent_MakeAvailable(libjpeg-turbo)
    set(IMG2JXL_JPEG_TARGET jpeg-static)
endif()

# ---------------------------------------------------------------------------
# extra image codecs: giflib (vendored subset), qoi (header-only),
# libwebp and libtiff (vendored, CMake)
# ---------------------------------------------------------------------------
add_library(giflib_v STATIC
    thirdparty/giflib/dgif_lib.c
    thirdparty/giflib/egif_lib.c
    thirdparty/giflib/gif_err.c
    thirdparty/giflib/gifalloc.c
    thirdparty/giflib/openbsd-reallocarray.c
    thirdparty/giflib/qprintf.c
    thirdparty/giflib/gif_hash.c
)
target_include_directories(giflib_v PUBLIC thirdparty/giflib)

add_library(qoi INTERFACE)
target_include_directories(qoi INTERFACE thirdparty/qoi)

set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/libwebp libwebp-build)

set(tiff-static ON CACHE BOOL "" FORCE)
set(tiff-tools OFF CACHE BOOL "" FORCE)
set(tiff-tests OFF CACHE BOOL "" FORCE)
set(tiff-contrib OFF CACHE BOOL "" FORCE)
set(tiff-docs OFF CACHE BOOL "" FORCE)
set(tiff-cxx OFF CACHE BOOL "" FORCE)
set(jpeg OFF CACHE BOOL "" FORCE)
set(old-jpeg OFF CACHE BOOL "" FORCE)
set(webp OFF CACHE BOOL "" FORCE)
set(zstd OFF CACHE BOOL "" FORCE)
set(lzma OFF CACHE BOOL "" FORCE)
set(jbig OFF CACHE BOOL "" FORCE)
set(pixarlog OFF CACHE BOOL "" FORCE)
set(lerc OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/tiff tiff-build)

# ---------------------------------------------------------------------------
# libjxl 0.12.0 — the JPEG XL encoder/decoder (vendored with its
# third_party/brotli, third_party/highway and third_party/skcms)
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)   # keeps libjxl from requiring GTest
set(JPEGXL_ENABLE_FUZZERS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DEVTOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DOXYGEN OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JNI OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_VIEWERS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_WASM_THREADS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_COVERAGE OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# skcms (vendored under libjxl/third_party/skcms) is the color management
set(JPEGXL_ENABLE_SKCMS ON CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TRANSCODE_JPEG ON CACHE BOOL "" FORCE)   # JPEG lossless transcoding
set(JPEGXL_ENABLE_BOXES ON CACHE BOOL "" FORCE)
# MinGW-w64 gcc miscompiles highway's AVX2 code paths (decoder crashes in
# the AVX2 output stage); disable them — SSE4 remains enabled
if(MINGW)
    set(JPEGXL_ENABLE_HWY_AVX2 OFF CACHE BOOL "" FORCE)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libjxl/CMakeLists.txt")
    message(STATUS "img2jxl: using vendored libjxl")
    add_subdirectory(thirdparty/libjxl libjxl-build)
elseif(LIBJXL_SOURCE_DIR)
    message(STATUS "img2jxl: using local libjxl at ${LIBJXL_SOURCE_DIR}")
    add_subdirectory(${LIBJXL_SOURCE_DIR} libjxl-build)
else()
    if(NOT IMG2JXL_FETCH_DEPS)
        message(FATAL_ERROR "img2jxl: no libjxl; vendor it under thirdparty/libjxl, pass -DLIBJXL_SOURCE_DIR, or enable IMG2JXL_FETCH_DEPS")
    endif()
    message(STATUS "img2jxl: fetching libjxl v0.12.0")
    FetchContent_Declare(libjxl
        URL https://github.com/libjxl/libjxl/archive/refs/tags/v0.12.0.tar.gz)
    FetchContent_MakeAvailable(libjxl)
endif()
# libjxl 0.12 targets are jxl / jxl_threads; with BUILD_SHARED_LIBS=OFF they
# build as static libraries (libjxl.a, libjxl_threads.a)
set(IMG2JXL_JXL_TARGET jxl)
set(IMG2JXL_JXL_THREADS_TARGET jxl_threads)

# ---------------------------------------------------------------------------
# img2jxl core
# ---------------------------------------------------------------------------
add_library(img2jxl_core STATIC
    src/image.c
    src/dec/bmp.c
    src/dec/tga.c
    src/dec/pnm.c
    src/dec/pngdec.c
    src/dec/ico.c
    src/dec/jpeg.c
    src/dec/gif.c
    src/dec/qoi.c
    src/dec/webp.c
    src/dec/tiff.c
    src/dec/jxldec.c
    src/enc/jxlenc.c
    src/convert.c
    src/util/threads.c
    src/util/filetime.c
)
target_include_directories(img2jxl_core PUBLIC src)
target_compile_definitions(img2jxl_core PUBLIC _CRT_SECURE_NO_WARNINGS)

# libjxl's static targets don't propagate their include dirs when built as a
# subproject; add the source and generated header dirs directly
target_include_directories(img2jxl_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libjxl/lib/include"
    "${CMAKE_BINARY_DIR}/libjxl-build/lib/include")

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(img2jxl_core PUBLIC Threads::Threads)
endif()

target_link_libraries(img2jxl_core PUBLIC
    ${IMG2JXL_JXL_TARGET} ${IMG2JXL_JXL_THREADS_TARGET}
    ${IMG2JXL_PNG_TARGET} ${IMG2JXL_JPEG_TARGET}
    giflib_v qoi webp tiff)

# libjpeg-turbo's targets don't carry PUBLIC include dirs (upstream expects
# an install layout), so add them directly for source-tree integrations
if(IMG2JXL_JPEG_TARGET STREQUAL "jpeg-static")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libjpeg-turbo/CMakeLists.txt")
        target_include_directories(img2jxl_core PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libjpeg-turbo/src"
            "${CMAKE_BINARY_DIR}/libjpeg-turbo-build")
    else()
        target_include_directories(img2jxl_core PUBLIC
            "${libjpeg-turbo_SOURCE_DIR}/src" "${libjpeg-turbo_BINARY_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
# libjxl's Modular mode (effort 10) recurses deeply; the MinGW default 2 MB
# stack overflows, so reserve more for every executable
if(MINGW OR CYGWIN)
    set(IMG2JXL_LINK_FLAGS -Wl,--stack,16777216)
endif()

add_executable(img2jxl src/cli/main.c)
target_link_options(img2jxl PRIVATE ${IMG2JXL_LINK_FLAGS})
target_link_libraries(img2jxl PRIVATE img2jxl_core)

# ---------------------------------------------------------------------------
# GUI (Qt6, optional)
# ---------------------------------------------------------------------------
if(IMG2JXL_GUI)
    find_package(Qt6 COMPONENTS Widgets)
    if(NOT Qt6_FOUND)
        message(STATUS "img2jxl: Qt6 not found, building CLI only (-DIMG2JXL_GUI=OFF to silence)")
        set(IMG2JXL_GUI OFF)
    else()
        # Qt < 6.3 (e.g. ubuntu-22.04 packages) does not provide this command,
        # so set up what it does for us explicitly:
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)
        if(COMMAND qt_standard_project_setup)
            qt_standard_project_setup()
        endif()
        if(APPLE)
            set(IMG2JXL_GUI_MACOSX_BUNDLE MACOSX_BUNDLE)
        endif()
        qt_add_executable(img2jxl-gui WIN32 ${IMG2JXL_GUI_MACOSX_BUNDLE}
            src/gui/main_gui.cpp
            src/gui/mainwindow.cpp
            src/gui/mainwindow.h
        )
        target_link_options(img2jxl-gui PRIVATE ${IMG2JXL_LINK_FLAGS})
        target_link_libraries(img2jxl-gui PRIVATE Qt6::Widgets img2jxl_core)
    endif()
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(IMG2JXL_TESTS)
    enable_testing()
    add_executable(img2jxl_selftest tests/selftest.c)
    target_link_options(img2jxl_selftest PRIVATE ${IMG2JXL_LINK_FLAGS})
    target_link_libraries(img2jxl_selftest PRIVATE img2jxl_core)
endif()
