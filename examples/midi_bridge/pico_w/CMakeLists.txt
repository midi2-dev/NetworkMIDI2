# NetworkMIDI2 <-> USB MIDI 2.0 bridge for Pico 2 W (RP2350 + cyw43 WiFi),
# USB DEVICE role, composite MIDI 2.0 (UMP) + CDC console.
#
# Release build — links NetworkMIDI2 core + the lwIP transport against the
# pre-built libs in this distribution (lib/pico/rp2350/) -- safe to reuse
# here because this target's lwipopts.h is byte-identical to
# examples/midi_bridge/lwip's (the example those libs were built from). USB
# MIDI support (TinyUSB via the Pico SDK itself, tusb_ump, AM_MIDI2.0Lib) is
# built from source from the third_party/ git submodules below.
#
# KNOWN ISSUE (as of pico-sdk 2.3.0/2.3.1, unresolved): the composite USB
# device (MIDI+CDC) built by this target does not enumerate on macOS on
# RP2350 -- see the comment further down and examples/midi_bridge/pico_w/README.md.
#
# Prerequisites:
#   PICO_SDK_PATH   path to Pico SDK 2.3.0     (default: ~/.pico-sdk/sdk/2.3.0)
#   PICO_BOARD      pico2_w (RP2350 + cyw43)    (default below)
#   third_party/ submodules -- run once after cloning this release repo:
#     git submodule update --init --recursive
#
# Quick-start (from _release/ directory):
#   git submodule update --init --recursive
#   cmake -B build_pico2w \
#         -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
#         -DPICO_BOARD=pico2_w \
#         examples/midi_bridge/pico_w
#   cmake --build build_pico2w -j6
#
# Flash: copy build_pico2w/nm2_bridge_pico_w.uf2 to the Pico 2 W in BOOTSEL mode.
# Console: USB CDC (composite with the MIDI interface) and UART, both live --
# use whichever is convenient; see README.md.

cmake_minimum_required(VERSION 3.18)

set(PICO_BOARD pico2_w CACHE STRING "Pico board target (default: pico2_w, RP2350+cyw43)")

set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Pico SDK root")
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.3.0")
    message(STATUS "PICO_SDK_PATH not set — defaulting to ${PICO_SDK_PATH}")
endif()

# Release root is three levels up: _release/examples/midi_bridge/pico_w/
get_filename_component(NM2_RELEASE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

include(${NM2_RELEASE_ROOT}/cmake/pico_sdk_import.cmake)

project(nm2_bridge_pico_w CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

pico_sdk_init()

# examples/midi_bridge/pico's usb_descriptors.cpp/device/tusb_config.h are
# shared as-is (board-agnostic composite MIDI+CDC descriptor).
set(PICO_ETH_EXAMPLE_DIR ${NM2_RELEASE_ROOT}/examples/midi_bridge/pico)
set(NM2_MDNS_NAME "pico2w-nm2-bridge" CACHE STRING "mDNS base name")

# ---------------------------------------------------------------------------
# third_party/ submodules (tusb_ump, AM_MIDI2.0Lib) -- public upstream
# projects vendored as git submodules in this release repo, not part of
# NetworkMIDI2's own proprietary source. Verify they were cloned.
# ---------------------------------------------------------------------------
foreach(_nm2_submodule_check "tusb_ump/ump_device.cpp" "AM_MIDI2.0Lib/CMakeLists.txt")
    if(NOT EXISTS "${NM2_RELEASE_ROOT}/third_party/${_nm2_submodule_check}")
        message(FATAL_ERROR
            "third_party/ submodules are missing or empty "
            "(expected ${_nm2_submodule_check}).\n"
            "Run from the release repo root: git submodule update --init --recursive")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# NetworkMIDI2 core — IMPORTED from pre-built release library
# ---------------------------------------------------------------------------
add_library(networkmidi2 STATIC IMPORTED)
set_target_properties(networkmidi2 PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/pico/rp2350/libnetworkmidi2.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/include"
)

# ---------------------------------------------------------------------------
# lwIP transport — IMPORTED from pre-built release library. Reusing the
# same lib/pico/rp2350 build as examples/midi_bridge/lwip is safe ONLY
# because this target's lwipopts.h is byte-identical to that example's
# (verified at release-packaging time) -- lwIP's compiled behavior is
# sensitive to lwipopts.h macros (MEM_SIZE, LWIP_TCP, pool sizes, etc.), so
# a different lwipopts.h would need its own pre-built variant instead of
# reusing this one.
# ---------------------------------------------------------------------------
add_library(nm2_transport_lwip STATIC IMPORTED GLOBAL)
set_target_properties(nm2_transport_lwip PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/pico/rp2350/libnm2_transport_lwip.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/transports/lwip;${NM2_RELEASE_ROOT}/include"
    INTERFACE_COMPILE_DEFINITIONS "NM2_HAVE_LWIP_MDNS=1"
    INTERFACE_LINK_LIBRARIES "networkmidi2;pico_stdlib;pico_cyw43_arch_lwip_poll;pico_lwip_mdns"
)

# ---------------------------------------------------------------------------
# AM_MIDI2.0Lib (third_party/AM_MIDI2.0Lib submodule) — UMP message helpers
# used by the DEVICE role's local Endpoint/Function Block Discovery replies.
# ---------------------------------------------------------------------------
add_subdirectory(${NM2_RELEASE_ROOT}/third_party/AM_MIDI2.0Lib ${CMAKE_CURRENT_BINARY_DIR}/AM_MIDI2.0Lib EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# tusb_ump — USB UMP class driver (third_party/tusb_ump submodule). Only the
# DEVICE-role source (ump_device.cpp) compiles to anything here; ump_host.cpp
# self-guards on CFG_TUH_ENABLED, unset in device/tusb_config.h.
# ---------------------------------------------------------------------------
file(GLOB TUSB_UMP_SRCS CONFIGURE_DEPENDS ${NM2_RELEASE_ROOT}/third_party/tusb_ump/*.cpp)

# ---------------------------------------------------------------------------
# Example executable
# ---------------------------------------------------------------------------
add_executable(nm2_bridge_pico_w
    main.cpp
    bridge_config.cpp
    console_menu.cpp
    ${PICO_ETH_EXAMPLE_DIR}/usb_descriptors.cpp   # composite MIDI(UMP)+CDC descriptors, shared as-is
    ${TUSB_UMP_SRCS}
)
target_include_directories(nm2_bridge_pico_w PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}        # lwipopts.h — MUST be first (see nm2_transport_lwip's comment above)
    ${PICO_ETH_EXAMPLE_DIR}/device   # device/tusb_config.h — reused from examples/midi_bridge/pico (CFG_TUD_CDC=1 default there)
    ${NM2_RELEASE_ROOT}/third_party/tusb_ump
    ${NM2_RELEASE_ROOT}/third_party/AM_MIDI2.0Lib
    ${NM2_RELEASE_ROOT}/include
    ${NM2_RELEASE_ROOT}/transports/lwip
)
target_compile_definitions(nm2_bridge_pico_w PRIVATE
    NM2_HAVE_LWIP_MDNS=1
    NM2_MDNS_NAME="${NM2_MDNS_NAME}"
)
target_compile_options(nm2_bridge_pico_w PRIVATE -Wno-narrowing -Wno-unused-variable)

target_link_libraries(nm2_bridge_pico_w PRIVATE
    nm2_transport_lwip
    networkmidi2
    pico_stdlib
    pico_unique_id
    tinyusb_device
    libmidi2
    pico_cyw43_arch_lwip_poll
    pico_lwip_mdns
)

# KNOWN ISSUE (as of pico-sdk 2.3.0/2.3.1, unresolved): on this Pico 2 W
# (RP2350) board, the composite USB device (MIDI+CDC) built by this target
# does not enumerate on macOS -- confirmed via extensive isolation that this
# is NOT a bug in this project's descriptors, tusb_ump, board/CMake config,
# or the tusb_init() API used. It reproduces with 100% stock TinyUSB
# descriptors/class driver too, and dcd_rp2040.c compiles to byte-identical
# object code in failing vs. working link configurations -- only the final
# link layout differs. This matches the class of bug tracked upstream at
# raspberrypi/pico-sdk#2216 ("USB low level example fails on RP2350 with
# extra endpoint added", open, milestone 2.4.0, compiler/link-layout
# sensitive). UART console / config menu / WiFi setup all work correctly on
# this target; only USB device enumeration is affected. Revisit once
# pico-sdk 2.4.0 ships.

# Composite console: USB CDC (part of the same composite USB device as the
# MIDI interface, see usb_descriptors.cpp) AND UART both enabled -- the SDK's
# native dual-stdio (pico_enable_stdio_usb + pico_enable_stdio_uart) handles
# routing printf()/getchar() across both without any custom _write() retarget
# code. CDC exists so boards without a Picoprobe/debug UART wired up still
# have a console; UART stays available for bench use with a probe.
pico_enable_stdio_uart(nm2_bridge_pico_w 1)
pico_enable_stdio_usb(nm2_bridge_pico_w 1)

pico_add_extra_outputs(nm2_bridge_pico_w)
