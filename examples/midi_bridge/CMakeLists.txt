# NetworkMIDI2 — POSIX desktop examples (macOS / Linux)
#
# Standalone project: builds against the pre-built NetworkMIDI2 binary
# distribution in the parent release directory.
#
# Quick-start (from the _release/ directory):
#
#   # macOS arm64
#   cmake -B build_examples/macos-arm64 examples/midi_bridge
#   cmake --build build_examples/macos-arm64 --parallel
#
#   # macOS x86_64
#   cmake -B build_examples/macos-x86_64 \
#         -DCMAKE_OSX_ARCHITECTURES=x86_64 \
#         examples/midi_bridge
#   cmake --build build_examples/macos-x86_64 --parallel
#
#   # Linux x86_64 (cross-compile from macOS)
#   cmake -B build_examples/linux-x86_64 \
#         -DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchain-linux-x86_64.cmake \
#         -DCMAKE_EXE_LINKER_FLAGS="-static" \
#         examples/midi_bridge
#   cmake --build build_examples/linux-x86_64 --parallel

cmake_minimum_required(VERSION 3.16)
project(nm2_examples CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Locate the NetworkMidi2 cmake config two directories above this file
# (_release/cmake/NetworkMidi2Config.cmake).
get_filename_component(NM2_RELEASE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
list(PREPEND CMAKE_PREFIX_PATH "${NM2_RELEASE_ROOT}")
find_package(NetworkMidi2 REQUIRED)

set(COMMON_SRCS
    common/DemoMidiSource.cpp
    common/MidiBridgeApp.cpp
)

add_executable(nm2_host        posix/main_host.cpp        ${COMMON_SRCS})
add_executable(nm2_client      posix/main_client.cpp      ${COMMON_SRCS})
add_executable(nm2_interactive posix/main_interactive.cpp common/DemoMidiSource.cpp)

foreach(tgt nm2_host nm2_client nm2_interactive)
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_link_libraries(${tgt} PRIVATE NetworkMidi2::nm2_transport_posix)
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-unused-parameter)
endforeach()
