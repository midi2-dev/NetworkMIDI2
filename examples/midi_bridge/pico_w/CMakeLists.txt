# NetworkMIDI2 <-> USB MIDI 2.0 bridge for Pico 2 W (RP2350 + cyw43 WiFi),
# USB DEVICE role, composite MIDI 2.0 (UMP) + CDC console. See README.md.
#
# NO_SYS=1 polling loop (no RTOS). WiFi credentials, session role, and
# device name are runtime configuration (bridge_config.h/console_menu.h,
# own copies for this target) set via the boot-time console menu and
# persisted to flash -- no build-time WiFi CMake cache vars.
#
# Prerequisites:
#   PICO_SDK_PATH   path to Pico SDK 2.3.0     (default: ~/.pico-sdk/sdk/2.3.0)
#   PICO_BOARD      pico2_w (RP2350 + cyw43)    (default below)
#
# Quick-start (from repo root):
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

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/pico_sdk_import.cmake)

project(nm2_bridge_pico_w CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

pico_sdk_init()

set(NM2_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(THIRD_PARTY_DIR ${NM2_ROOT}/third_party)
set(PICO_ETH_EXAMPLE_DIR ${CMAKE_CURRENT_LIST_DIR}/../pico)   # usb_descriptors.cpp, device/tusb_config.h — shared, board-agnostic

set(NM2_MDNS_NAME "pico2w-nm2-bridge" CACHE STRING "mDNS base name")

# ---------------------------------------------------------------------------
# NetworkMIDI2 core + lwIP transport — built from source (this project IS
# NetworkMIDI2).
# ---------------------------------------------------------------------------
add_library(networkmidi2_pico_w STATIC
    ${NM2_ROOT}/src/Protocol.cpp
    ${NM2_ROOT}/src/NetworkMidiSession.cpp
    ${NM2_ROOT}/src/SessionAuth.cpp
    ${NM2_ROOT}/src/Sha256.cpp
    ${NM2_ROOT}/src/SharedSecretAuthenticator.cpp
)
target_include_directories(networkmidi2_pico_w
    PUBLIC  ${NM2_ROOT}/include
    PRIVATE ${NM2_ROOT}/src
)
target_compile_options(networkmidi2_pico_w PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_library(nm2_transport_lwip_pico_w STATIC
    ${NM2_ROOT}/transports/lwip/LwipUdpTransport.cpp
    ${NM2_ROOT}/transports/lwip/LwipMdnsDiscovery.cpp
)
target_include_directories(nm2_transport_lwip_pico_w PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}        # lwipopts.h (SDK default is fine here — no custom one needed)
    ${NM2_ROOT}/transports/lwip
    ${NM2_ROOT}/include
)
target_compile_definitions(nm2_transport_lwip_pico_w PUBLIC NM2_HAVE_LWIP_MDNS=1)
target_compile_options(nm2_transport_lwip_pico_w PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nm2_transport_lwip_pico_w PUBLIC
    networkmidi2_pico_w
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_lwip_mdns
)

# ---------------------------------------------------------------------------
# AM_MIDI2.0Lib (third_party/AM_MIDI2.0Lib submodule) — UMP message helpers
# used by the DEVICE role's local Endpoint/Function Block Discovery replies.
# ---------------------------------------------------------------------------
add_subdirectory(${THIRD_PARTY_DIR}/AM_MIDI2.0Lib ${CMAKE_CURRENT_BINARY_DIR}/AM_MIDI2.0Lib EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# tusb_ump — USB UMP class driver (third_party/tusb_ump submodule). Only the
# DEVICE-role source (ump_device.cpp) compiles to anything here; ump_host.cpp
# self-guards on CFG_TUH_ENABLED, unset in device/tusb_config.h.
# ---------------------------------------------------------------------------
file(GLOB TUSB_UMP_SRCS CONFIGURE_DEPENDS ${THIRD_PARTY_DIR}/tusb_ump/*.cpp)

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
    ${PICO_ETH_EXAMPLE_DIR}/device   # device/tusb_config.h — reused from examples/midi_bridge/pico (CFG_TUD_CDC=1 already set there)
    ${THIRD_PARTY_DIR}/tusb_ump
    ${THIRD_PARTY_DIR}/AM_MIDI2.0Lib
    ${NM2_ROOT}/include
    ${NM2_ROOT}/transports/lwip
)
target_compile_definitions(nm2_bridge_pico_w PRIVATE
    NM2_HAVE_LWIP_MDNS=1
    NM2_MDNS_NAME="${NM2_MDNS_NAME}"
)
target_compile_options(nm2_bridge_pico_w PRIVATE -Wno-narrowing -Wno-unused-variable)

target_link_libraries(nm2_bridge_pico_w PRIVATE
    nm2_transport_lwip_pico_w
    networkmidi2_pico_w
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
# sensitive). A minimal repro happened to be fixed by additionally linking
# tinyusb_board (unused -- board_init() is never called), but that same
# trick did NOT fix this larger application, confirming the underlying
# issue is link-layout-sensitive rather than something addressable with a
# specific flag or library here. UART console / config menu / WiFi setup
# all work correctly on this target; only USB device enumeration is
# affected. Revisit once pico-sdk 2.4.0 ships (or after porting
# third_party/tusb_ump to a newer upstream TinyUSB that includes whatever
# fix eventually lands for #2216).

# Composite console: USB CDC (part of the same composite USB device as the
# MIDI interface, see usb_descriptors.cpp) AND UART both enabled -- the SDK's
# native dual-stdio (pico_enable_stdio_usb + pico_enable_stdio_uart) handles
# routing printf()/getchar() across both without any custom _write() retarget
# code. CDC exists so boards without a Picoprobe/debug UART wired up still
# have a console; UART stays available for bench use with a probe.
pico_enable_stdio_uart(nm2_bridge_pico_w 1)
pico_enable_stdio_usb(nm2_bridge_pico_w 1)

pico_add_extra_outputs(nm2_bridge_pico_w)
