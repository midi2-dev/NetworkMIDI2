# NetworkMIDI2 <-> USB MIDI 2.0 bridge for Pico 2 W (RP2350 + cyw43 WiFi).
# Two USB roles via NM2_BRIDGE_USB_ROLE (HOST/DEVICE, default DEVICE) -- see
# examples/midi_bridge/pico_w/README.md.
#
# Release build — links NetworkMIDI2 core + the lwIP transport against the
# pre-built libs in this distribution (lib/pico/rp2350/) -- safe to reuse
# here because this target's lwipopts.h is byte-identical to
# examples/midi_bridge/lwip's (the example those libs were built from). USB
# MIDI support (TinyUSB via the Pico SDK itself, tusb_ump, AM_MIDI2.0Lib) is
# built from source from the third_party/ git submodules below.
#
# Prerequisites:
#   PICO_SDK_PATH   path to Pico SDK 2.3.0     (default: ~/.pico-sdk/sdk/2.3.0)
#   PICO_BOARD      pico2_w (RP2350 + cyw43)    (default below)
#   third_party/ submodules -- run once after cloning this release repo:
#     git submodule update --init --recursive
#
# Quick-start (from _release/ directory):
#   git submodule update --init --recursive
#   # DEVICE role (default):
#   cmake -B build_pico2w \
#         -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
#         -DPICO_BOARD=pico2_w \
#         examples/midi_bridge/pico_w
#   cmake --build build_pico2w -j6
#   # HOST role:
#   cmake -B build_pico2w_host \
#         -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
#         -DPICO_BOARD=pico2_w \
#         -DNM2_BRIDGE_USB_ROLE=HOST \
#         examples/midi_bridge/pico_w
#   cmake --build build_pico2w_host -j6
#
# Flash: copy build_pico2w/nm2_bridge_pico_w.uf2 to the Pico 2 W in BOOTSEL mode.
# Console: DEVICE role -- USB CDC (composite with the MIDI interface) and
# UART, both live, use whichever is convenient. HOST role -- UART only (USB
# port is occupied being the host port). See README.md.

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

# examples/midi_bridge/pico's usb_descriptors.cpp/device/tusb_config.h
# (DEVICE role) and host/tusb_config.h + tinyusb_overrides/hcd_rp2040.c
# (HOST role) are shared as-is (board-agnostic).
set(PICO_ETH_EXAMPLE_DIR ${NM2_RELEASE_ROOT}/examples/midi_bridge/pico)
set(NM2_MDNS_NAME "pico2w-nm2-bridge" CACHE STRING "mDNS base name")

# USB role: DEVICE (default) or HOST -- see README.md.
set(NM2_BRIDGE_USB_ROLE "DEVICE" CACHE STRING "USB role: HOST or DEVICE")
set_property(CACHE NM2_BRIDGE_USB_ROLE PROPERTY STRINGS HOST DEVICE)
if(NM2_BRIDGE_USB_ROLE STREQUAL "HOST")
    set(NM2_BRIDGE_USB_HOST 1)
else()
    set(NM2_BRIDGE_USB_HOST 0)
endif()

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
# active role's source compiles to anything -- each self-guards on
# CFG_TUD_ENABLED/CFG_TUH_ENABLED, unset in the inactive role's tusb_config.h.
# ---------------------------------------------------------------------------
file(GLOB TUSB_UMP_SRCS CONFIGURE_DEPENDS ${NM2_RELEASE_ROOT}/third_party/tusb_ump/*.cpp)

# usb_descriptors.cpp is DEVICE-role only (references CFG_TUD_* macros that
# don't exist in the HOST role's tusb_config.h).
set(NM2_BRIDGE_SRC main.cpp bridge_config.cpp console_menu.cpp ${TUSB_UMP_SRCS})
if(NOT NM2_BRIDGE_USB_HOST)
    list(APPEND NM2_BRIDGE_SRC ${PICO_ETH_EXAMPLE_DIR}/usb_descriptors.cpp)
endif()

# ---------------------------------------------------------------------------
# Example executable
# ---------------------------------------------------------------------------
add_executable(nm2_bridge_pico_w ${NM2_BRIDGE_SRC})

if(NM2_BRIDGE_USB_HOST)
    # host/tusb_config.h (reused from examples/midi_bridge/pico) must be
    # found ahead of anything else that might provide a device-mode one.
    target_include_directories(nm2_bridge_pico_w PRIVATE
        ${PICO_ETH_EXAMPLE_DIR}/host
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040
    )
else()
    target_include_directories(nm2_bridge_pico_w PRIVATE
        ${PICO_ETH_EXAMPLE_DIR}/device   # device/tusb_config.h — reused from examples/midi_bridge/pico (CFG_TUD_CDC=1 default there)
    )
endif()

target_include_directories(nm2_bridge_pico_w PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}        # lwipopts.h — needed by both roles (see nm2_transport_lwip's comment above)
    ${NM2_RELEASE_ROOT}/third_party/tusb_ump
    ${NM2_RELEASE_ROOT}/third_party/AM_MIDI2.0Lib
    ${NM2_RELEASE_ROOT}/include
    ${NM2_RELEASE_ROOT}/transports/lwip
)
target_compile_definitions(nm2_bridge_pico_w PRIVATE
    NM2_HAVE_LWIP_MDNS=1
    NM2_MDNS_NAME="${NM2_MDNS_NAME}"
    NM2_BRIDGE_USB_HOST=${NM2_BRIDGE_USB_HOST}
)
target_compile_options(nm2_bridge_pico_w PRIVATE -Wno-narrowing -Wno-unused-variable)

# DEVICE role only: RP2350 fixes the RP2040-E15 bulk-IN uframe errata in
# hardware, but pico-sdk 2.3.0's bundled TinyUSB (0.18.0) force-applies the
# E15 workaround to every tinyusb_device consumer regardless of chip.
# Upstream TinyUSB already gates this to rp2040 only (commit 0b970e6).
if(NOT NM2_BRIDGE_USB_HOST)
    target_compile_definitions(nm2_bridge_pico_w PRIVATE
        TUD_OPT_RP2040_USB_DEVICE_UFRAME_FIX=0
        PICO_RP2040_USB_FAST_IRQ=1
        # tud_task() is pumped manually (main.cpp's usb_task_tick(), see
        # README.md's "USB service architecture") instead of via
        # pico_stdio_usb's IRQ-driven background task -- that combination
        # (untested elsewhere in this project) previously raced against
        # WiFi connect and caused enumeration to fail. See README.md.
        PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0
    )
endif()

target_link_libraries(nm2_bridge_pico_w PRIVATE
    nm2_transport_lwip
    networkmidi2
    pico_stdlib
    pico_unique_id
    $<IF:$<BOOL:${NM2_BRIDGE_USB_HOST}>,tinyusb_host,tinyusb_device>
    libmidi2
    pico_cyw43_arch_lwip_poll
    pico_lwip_mdns
)

if(NM2_BRIDGE_USB_HOST)
    # Swap in the patched hcd_rp2040.c (reused from examples/midi_bridge/pico
    # -- see that file's header comment), scoped to this target only. Pico 2
    # W's RP2350 has no separate TinyUSB portable driver, only the shared
    # rp2040 one.
    target_sources(nm2_bridge_pico_w PRIVATE ${PICO_ETH_EXAMPLE_DIR}/tinyusb_overrides/hcd_rp2040.c)
    get_target_property(_tinyusb_host_base_srcs tinyusb_host_base INTERFACE_SOURCES)
    list(FILTER _tinyusb_host_base_srcs EXCLUDE REGEX "portable/raspberrypi/rp2040/hcd_rp2040\\.c$")
    set_target_properties(tinyusb_host_base PROPERTIES INTERFACE_SOURCES "${_tinyusb_host_base_srcs}")
endif()

# DEVICE role: composite console -- USB CDC (part of the same composite USB
# device as the MIDI interface, see usb_descriptors.cpp) AND UART both
# enabled, via the SDK's native dual-stdio (pico_enable_stdio_usb +
# pico_enable_stdio_uart). CDC exists so boards without a Picoprobe/debug
# UART wired up still have a console; UART stays available for bench use
# with a probe.
# HOST role: UART only -- the USB port is occupied being the host port.
pico_enable_stdio_uart(nm2_bridge_pico_w 1)
if(NM2_BRIDGE_USB_HOST)
    pico_enable_stdio_usb(nm2_bridge_pico_w 0)
else()
    pico_enable_stdio_usb(nm2_bridge_pico_w 1)
endif()

pico_add_extra_outputs(nm2_bridge_pico_w)
