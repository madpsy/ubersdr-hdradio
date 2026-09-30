# ubersdr-hdradio: HD Radio (NRSC-5) AM decoder for UberSDR.
#
# One executable, spawned by UberSDR's hdradio audio extension: IQ on stdin,
# audio on stdout, JSON Lines status on fd 3, commands on fd 4 (src/main.cpp).
#
# The decoder is nrsc5's library, compiled here from a pinned commit with the
# patches in patches/ and linked in statically, with its rtl-sdr and rtl_tcp
# calls stubbed out (samples come from stdin, never from a dongle). Its AAC
# decoding needs FAAD2 with nrsc5's HDC patch, which no distribution ships, so
# that is fetched, patched and linked in statically too. FFTW is linked
# statically as well, so the binary needs nothing beyond libc and libstdc++.

cmake_minimum_required(VERSION 3.24)
project(ubersdr-hdradio C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif ()

include(FetchContent)
# FetchContent_Populate is used on purpose: nrsc5's own CMake (which wants
# rtl-sdr, libusb and autotools) is not used at all.
if (POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif ()

# --- nrsc5 --------------------------------------------------------------------
set(NRSC5_COMMIT 9beb2c77171472c707f285f51a1385ac4c4a8be6)
set(NRSC5_SOURCE_DIR "" CACHE PATH "nrsc5 source tree (default: fetch the pinned commit)")
if (NOT NRSC5_SOURCE_DIR)
    FetchContent_Declare(nrsc5
        URL https://github.com/theori-io/nrsc5/archive/${NRSC5_COMMIT}.tar.gz
        URL_HASH SHA256=ebcf9b0acbb72d27c53e65c474929df7283f5866939a728e48d5a7508c61f9bf
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(nrsc5)
    if (NOT nrsc5_POPULATED)
        FetchContent_Populate(nrsc5)
    endif ()
    set(NRSC5_SOURCE_DIR ${nrsc5_SOURCE_DIR})
endif ()
if (NOT EXISTS "${NRSC5_SOURCE_DIR}/src/nrsc5.c")
    message(FATAL_ERROR "NRSC5_SOURCE_DIR (${NRSC5_SOURCE_DIR}) is not an nrsc5 source tree")
endif ()

# --- FAAD2, patched for HDC -----------------------------------------------------
FetchContent_Declare(faad2
    URL https://github.com/knik0/faad2/archive/refs/tags/2.11.2.tar.gz
    URL_HASH SHA256=3fcbd305e4abd34768c62050e18ca0986f7d9c5eca343fb98275418013065c0e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(faad2)
if (NOT faad2_POPULATED)
    FetchContent_Populate(faad2)
    if (NOT EXISTS "${faad2_SOURCE_DIR}/.hdc_patched")
        find_program(PATCH_PROGRAM patch REQUIRED)
        execute_process(
            COMMAND ${PATCH_PROGRAM} -p1 -N -i "${NRSC5_SOURCE_DIR}/support/faad2-hdc-support.patch"
            WORKING_DIRECTORY "${faad2_SOURCE_DIR}"
            RESULT_VARIABLE _patch_result)
        if (NOT _patch_result EQUAL 0)
            message(FATAL_ERROR "nrsc5's FAAD2 HDC patch did not apply")
        endif ()
        file(TOUCH "${faad2_SOURCE_DIR}/.hdc_patched")
    endif ()
    set(FAAD_BUNDLED_MODE ON CACHE BOOL "" FORCE)
    set(FAAD_BUILD_CLI OFF CACHE BOOL "" FORCE)
    add_subdirectory(${faad2_SOURCE_DIR} ${faad2_BINARY_DIR} EXCLUDE_FROM_ALL)
endif ()

# --- FFTW (single precision), static ---------------------------------------------
find_path(FFTW3F_INCLUDE_DIR fftw3.h REQUIRED)
find_library(FFTW3F_LIBRARY NAMES libfftw3f.a fftw3f REQUIRED)

# --- libnrsc5, static -------------------------------------------------------------
include(CheckSymbolExists)
set(CMAKE_REQUIRED_FLAGS --std=gnu11)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(strndup string.h HAVE_STRNDUP)
check_symbol_exists(CMPLXF complex.h HAVE_CMPLXF)
check_symbol_exists(_Imaginary_I complex.h HAVE_IMAGINARY_I)
check_symbol_exists(_Complex_I complex.h HAVE_COMPLEX_I)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_DEFINITIONS)
set(USE_FAAD2 ON)
set(LIBRARY_DEBUG_LEVEL 5)  # nrsc5's own logging off: stderr is for state changes
configure_file(${NRSC5_SOURCE_DIR}/src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/nrsc5/config.h)

# patches/nrsc5-*.patch change nrsc5 (see each patch); they are applied to a
# copy in the build tree, so a checkout given as NRSC5_SOURCE_DIR is never
# touched.
set(NRSC5_PATCHED_DIR ${CMAKE_CURRENT_BINARY_DIR}/nrsc5-patched)
file(GLOB NRSC5_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/nrsc5-*.patch)
set(_nrsc5_patch_stamp "${NRSC5_SOURCE_DIR};${NRSC5_PATCHES}")
foreach(_p ${NRSC5_PATCHES})
    file(SHA256 ${_p} _h)
    string(APPEND _nrsc5_patch_stamp ";${_h}")
endforeach()
if (NOT EXISTS ${NRSC5_PATCHED_DIR}/.stamp OR NOT "${_nrsc5_patch_stamp}" STREQUAL "${NRSC5_PATCH_STAMP_DONE}")
    file(REMOVE_RECURSE ${NRSC5_PATCHED_DIR})
    # The whole of src/, not just the patched files: nrsc5 includes its headers
    # with quotes, which look beside the including file first.
    file(GLOB _nrsc5_src ${NRSC5_SOURCE_DIR}/src/*.c ${NRSC5_SOURCE_DIR}/src/*.h)
    file(COPY ${_nrsc5_src} DESTINATION ${NRSC5_PATCHED_DIR})
    find_program(PATCH_PROGRAM patch REQUIRED)
    foreach(_p ${NRSC5_PATCHES})
        execute_process(COMMAND ${PATCH_PROGRAM} -p2 -N -i ${_p} WORKING_DIRECTORY ${NRSC5_PATCHED_DIR} RESULT_VARIABLE _r)
        if (NOT _r EQUAL 0)
            message(FATAL_ERROR "${_p} did not apply to nrsc5")
        endif ()
    endforeach()
    file(TOUCH ${NRSC5_PATCHED_DIR}/.stamp)
    set(NRSC5_PATCH_STAMP_DONE "${_nrsc5_patch_stamp}" CACHE INTERNAL "")
endif ()

set(NRSC5_SOURCES acquire.c decode.c frame.c here_images.c input.c nrsc5.c output.c pids.c
    sync.c firdecim_cf32.c conv_dec.c rs_init.c rs_decode.c unicode.c strndup.c)
list(TRANSFORM NRSC5_SOURCES PREPEND ${NRSC5_PATCHED_DIR}/)
add_library(nrsc5_static STATIC ${NRSC5_SOURCES} src/rtlsdr_stub.c src/rtltcp_stub.c)
target_include_directories(nrsc5_static
    PUBLIC ${NRSC5_SOURCE_DIR}/include
    PRIVATE ${NRSC5_PATCHED_DIR} ${CMAKE_CURRENT_BINARY_DIR}/nrsc5 compat/rtlsdr ${FFTW3F_INCLUDE_DIR})
string(SUBSTRING ${NRSC5_COMMIT} 0 7 _nrsc5_short)
target_compile_definitions(nrsc5_static PRIVATE _GNU_SOURCE HAVE_FAAD2 GIT_COMMIT_HASH="${_nrsc5_short}")
target_compile_options(nrsc5_static PRIVATE --std=gnu11 -O3)
# nrsc5's Viterbi decoder has SSE and NEON versions.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
    target_compile_options(nrsc5_static PRIVATE -msse2 -msse3 -mssse3)
    target_compile_definitions(nrsc5_static PRIVATE HAVE_SSE2 HAVE_SSE3)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64)")
    target_compile_definitions(nrsc5_static PRIVATE HAVE_NEON)
endif ()
target_link_libraries(nrsc5_static PUBLIC faad_hdc ${FFTW3F_LIBRARY} m pthread)

# --- the binary --------------------------------------------------------------------
add_executable(ubersdr-hdradio src/main.cpp src/hd_decoder.cpp)
target_include_directories(ubersdr-hdradio PRIVATE src)
target_compile_options(ubersdr-hdradio PRIVATE -Wall -Wextra)
target_link_libraries(ubersdr-hdradio PRIVATE nrsc5_static)
# libstdc++ and libgcc too: then the binary runs on any glibc as new as the one
# it was built against, whatever C++ runtime the container has.
target_link_options(ubersdr-hdradio PRIVATE -static-libstdc++ -static-libgcc)

# Architecture-suffixed output name in Go's GOARCH spellings, so the wrapper
# finds its binary with runtime.GOARCH and the Dockerfile with ${TARGETARCH}.
# Keyed on CMAKE_SYSTEM_PROCESSOR rather than `uname -m`, which reports the
# host under a cross toolchain.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(UBERSDR_ARCH "amd64")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(UBERSDR_ARCH "arm64")
else ()
    set(UBERSDR_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    message(WARNING "Unrecognised CMAKE_SYSTEM_PROCESSOR '${CMAKE_SYSTEM_PROCESSOR}'; "
                    "binary will be named ubersdr-hdradio_${UBERSDR_ARCH}")
endif ()
set_target_properties(ubersdr-hdradio PROPERTIES OUTPUT_NAME "ubersdr-hdradio_${UBERSDR_ARCH}")
message(STATUS "Binary will be built as ubersdr-hdradio_${UBERSDR_ARCH}")

install(TARGETS ubersdr-hdradio DESTINATION /opt/ubersdr-hdradio)

# --- tests ------------------------------------------------------------------------
#
#   ctest --test-dir build --output-on-failure
#
# resampler_test checks the resampler on its own. Each line of test/samples.txt
# plays a recording from testdata/ through the built binary, as UberSDR would
# feed it, and checks the audio and status that come out (test/check_sample.py).
enable_testing()
add_executable(resampler_test test/resampler_test.cpp)
target_include_directories(resampler_test PRIVATE src)
add_test(NAME resampler COMMAND resampler_test)

find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_test(NAME samples
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/check_sample.py
                $<TARGET_FILE:ubersdr-hdradio> ${CMAKE_CURRENT_SOURCE_DIR}/test/samples.txt)
    set_tests_properties(samples PROPERTIES TIMEOUT 600)
endif ()
