# NetworkMIDI2 — lwIP polling example for Pico 2 W
#
# Release build — links against pre-built libs in the _release/ directory.
# No NetworkMIDI2 source code required.
#
# Prerequisites:
#   PICO_SDK_PATH   path to Pico SDK 2.2.0  (default: ~/.pico-sdk/sdk/2.2.0)
#   PICO_BOARD      must be pico2_w          (default below)
#
# Quick-start (from _release/ directory):
#   cmake -B build_pico/rp2350 \
#         -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0 \
#         -DPICO_BOARD=pico2_w \
#         examples/midi_bridge/lwip
#   cmake --build build_pico/rp2350 -j4
#
# Flash: copy build_pico/rp2350/nm2_pico.uf2 to the Pico 2 W in BOOTSEL mode.
# Console: screen /dev/cu.usbmodem<id> 115200   (USB CDC, no hardware flow)

cmake_minimum_required(VERSION 3.16)

set(PICO_BOARD pico2_w CACHE STRING "Pico board target (default: pico2_w)")

set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Pico SDK root")
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.2.0")
    message(STATUS "PICO_SDK_PATH not set — defaulting to ${PICO_SDK_PATH}")
endif()

# Release root is three levels up from this file: _release/examples/midi_bridge/lwip/
get_filename_component(NM2_RELEASE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

include(${NM2_RELEASE_ROOT}/cmake/pico_sdk_import.cmake)

project(nm2_pico_lwip CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

pico_sdk_init()

# ---------------------------------------------------------------------------
# NetworkMIDI2 core — IMPORTED from pre-built release library
# ---------------------------------------------------------------------------
add_library(networkmidi2 STATIC IMPORTED)
set_target_properties(networkmidi2 PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/pico/rp2350/libnetworkmidi2.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/include"
)

# ---------------------------------------------------------------------------
# lwIP transport — IMPORTED from pre-built release library
#
# IMPORTANT: ${CMAKE_CURRENT_LIST_DIR} (which holds lwipopts.h) must appear
# FIRST in the include path when building the application.  The IMPORTED target
# exposes the headers from the release directory; the app adds lwipopts.h first
# via its own target_include_directories call.
# ---------------------------------------------------------------------------
add_library(nm2_transport_lwip STATIC IMPORTED GLOBAL)
set_target_properties(nm2_transport_lwip PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/pico/rp2350/libnm2_transport_lwip.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/transports/lwip;${NM2_RELEASE_ROOT}/include"
    INTERFACE_COMPILE_DEFINITIONS "NM2_HAVE_LWIP_MDNS=1"
    INTERFACE_LINK_LIBRARIES "networkmidi2;pico_stdlib;pico_cyw43_arch_lwip_poll;pico_lwip_mdns"
)

# ---------------------------------------------------------------------------
# Example executable
# ---------------------------------------------------------------------------
add_executable(nm2_pico
    main.cpp
    ../common/DemoMidiSource.cpp
)

# lwipopts.h MUST be the first include directory so lwIP picks it up.
target_include_directories(nm2_pico PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}           # lwipopts.h — first
    ${NM2_RELEASE_ROOT}/include
    ${NM2_RELEASE_ROOT}/transports/lwip
    ${CMAKE_CURRENT_LIST_DIR}/../common
)
target_compile_definitions(nm2_pico PRIVATE
    NM2_HAVE_LWIP_MDNS=1
)
target_compile_options(nm2_pico PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nm2_pico PRIVATE
    nm2_transport_lwip
    networkmidi2
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_lwip_mdns
)

pico_enable_stdio_usb(nm2_pico 1)
pico_enable_stdio_uart(nm2_pico 0)
pico_add_extra_outputs(nm2_pico)
