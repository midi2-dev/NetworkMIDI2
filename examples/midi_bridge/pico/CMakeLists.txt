# NetworkMIDI2 — USB MIDI 2.0 <-> NetworkMIDI2/W5500-Ethernet bridge for
# plain Pico (RP2040), ported from AmeNote ProtoZOA's
# UUT/NetworkMIDI2_Bridge. See README.md for the NM2_BRIDGE_USB_ROLE
# (HOST/DEVICE) and NM2_WIZNET_BOARD (PROTOZOA/W5500_EVB_PICO) options.
#
# NO_SYS=1 polling loop (no RTOS), same shape as examples/midi_bridge/lwip,
# but this variant is Ethernet (Wiznet W5500 over SPI) rather than WiFi.
#
# Prerequisites:
#   PICO_SDK_PATH   path to Pico SDK 2.3.0   (default: ~/.pico-sdk/sdk/2.3.0)
#   PICO_BOARD      plain "pico" (RP2040)     (default below)
#
# Quick-start (from repo root, USB HOST role, default wiring):
#   cmake -B build_pico \
#         -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
#         -DPICO_BOARD=pico \
#         examples/midi_bridge/pico
#   cmake --build build_pico -j6
#
# Flash: copy build_pico/nm2_bridge_pico.uf2 to the Pico held in BOOTSEL mode.
# Console: UART only (the USB port is occupied by the bridge's USB role) —
# see README.md for wiring/baud rate.

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

cmake_minimum_required(VERSION 3.18)

set(PICO_BOARD pico CACHE STRING "Pico board target (default: pico, RP2040)")

set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Pico SDK root")
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.3.0")
    message(STATUS "PICO_SDK_PATH not set — defaulting to ${PICO_SDK_PATH}")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/pico_sdk_import.cmake)

project(nm2_bridge_pico CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

pico_sdk_init()

set(NM2_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(THIRD_PARTY_DIR ${NM2_ROOT}/third_party)

# ---------------------------------------------------------------------------
# Wiznet ioLibrary_Driver (third_party/ioLibrary_Driver submodule) — W5500.
# ---------------------------------------------------------------------------
set(WIZNET_DIR ${THIRD_PARTY_DIR}/ioLibrary_Driver)
set(WIZNET_PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/wiznet_port)

# PROTOZOA (default): AmeNote ProtoZOA UUT J13 expansion pins (see
# wiznet_port/w5x00_spi.h). W5500_EVB_PICO: WIZnet's own W5500-EVB-Pico eval
# board instead — useful to isolate a software bug from board-specific wiring.
set(NM2_WIZNET_BOARD "PROTOZOA" CACHE STRING "Wiznet SPI pin set: PROTOZOA or W5500_EVB_PICO")
set_property(CACHE NM2_WIZNET_BOARD PROPERTY STRINGS PROTOZOA W5500_EVB_PICO)

# USB role: HOST (default — this bridge is itself the USB host, bridging a
# directly-/hub-attached USB MIDI device) or DEVICE (bridge presents as a
# USB MIDI device to a host computer/DAW, answering Endpoint/Function Block
# Discovery locally). Build-time choice, not runtime — different TinyUSB
# stack and tusb_config.h. See README.md.
set(NM2_BRIDGE_USB_ROLE "HOST" CACHE STRING "USB role: HOST or DEVICE")
set_property(CACHE NM2_BRIDGE_USB_ROLE PROPERTY STRINGS HOST DEVICE)
if(NM2_BRIDGE_USB_ROLE STREQUAL "HOST")
    set(NM2_BRIDGE_USB_HOST 1)
else()
    set(NM2_BRIDGE_USB_HOST 0)
endif()

add_library(nm2_bridge_wiznet_ethernet STATIC)
target_sources(nm2_bridge_wiznet_ethernet PUBLIC
    ${WIZNET_DIR}/Ethernet/socket.c
    ${WIZNET_DIR}/Ethernet/wizchip_conf.c
    ${WIZNET_DIR}/Ethernet/W5500/w5500.c
)
target_include_directories(nm2_bridge_wiznet_ethernet PUBLIC
    ${WIZNET_DIR}/Ethernet
    ${WIZNET_DIR}/Ethernet/W5500
)
target_compile_definitions(nm2_bridge_wiznet_ethernet PUBLIC _WIZCHIP_=W5500)

# w5x00_spi.c / w5x00_lwip.c: WIZnet SPI HAL + lwIP netif glue, vendored and
# customized for ProtoZOA's J13 pin-out — see wiznet_port/w5x00_spi.h.
add_library(nm2_bridge_wiznet_port STATIC)
target_sources(nm2_bridge_wiznet_port PUBLIC
    ${WIZNET_PORT_DIR}/w5x00_spi.c
    ${WIZNET_PORT_DIR}/w5x00_lwip.c
)
target_include_directories(nm2_bridge_wiznet_port PUBLIC
    ${WIZNET_PORT_DIR}
    ${WIZNET_DIR}/Ethernet
)
if(NM2_WIZNET_BOARD STREQUAL "W5500_EVB_PICO")
    target_compile_definitions(nm2_bridge_wiznet_port PUBLIC NM2_WIZNET_BOARD_W5500_EVB_PICO=1)
endif()
target_link_libraries(nm2_bridge_wiznet_port PUBLIC
    nm2_bridge_wiznet_ethernet
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_clocks
    pico_lwip
    pico_lwip_nosys
)

# lwipopts.h must be visible to every pico_lwip consumer (WIZnet port code
# and NetworkMIDI2's LwipUdpTransport both build against the same lwIP
# config). pico_lwip is a build-global CMake target; guard against a second
# app in the same configure setting a conflicting lwipopts.h.
get_property(_pico_lwip_opts_owner GLOBAL PROPERTY PICO_LWIP_OPTS_DIR_OWNER)
if(_pico_lwip_opts_owner AND NOT _pico_lwip_opts_owner STREQUAL CMAKE_CURRENT_LIST_DIR)
    message(FATAL_ERROR
        "pico_lwip's INTERFACE include directories were already set by "
        "${_pico_lwip_opts_owner} (a different lwipopts.h). pico_lwip is a "
        "single build-global target and cannot serve two conflicting lwIP "
        "configs in the same configure.")
endif()
set_property(GLOBAL PROPERTY PICO_LWIP_OPTS_DIR_OWNER ${CMAKE_CURRENT_LIST_DIR})
target_include_directories(pico_lwip INTERFACE ${WIZNET_PORT_DIR})

# ---------------------------------------------------------------------------
# NetworkMIDI2 core + lwIP transport — built from source (this project IS
# NetworkMIDI2), unlike ProtoZOA's binary-distribution consumption.
# ---------------------------------------------------------------------------
add_library(networkmidi2_pico STATIC
    ${NM2_ROOT}/src/Protocol.cpp
    ${NM2_ROOT}/src/NetworkMidiSession.cpp
    ${NM2_ROOT}/src/SessionAuth.cpp
    ${NM2_ROOT}/src/Sha256.cpp
    ${NM2_ROOT}/src/SharedSecretAuthenticator.cpp
)
target_include_directories(networkmidi2_pico
    PUBLIC  ${NM2_ROOT}/include
    PRIVATE ${NM2_ROOT}/src
)
target_compile_options(networkmidi2_pico PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_library(nm2_transport_lwip_pico STATIC
    ${NM2_ROOT}/transports/lwip/LwipUdpTransport.cpp
    ${NM2_ROOT}/transports/lwip/LwipMdnsDiscovery.cpp
)
target_include_directories(nm2_transport_lwip_pico PUBLIC
    ${WIZNET_PORT_DIR}               # lwipopts.h — must be first
    ${NM2_ROOT}/transports/lwip
    ${NM2_ROOT}/include
)
target_compile_definitions(nm2_transport_lwip_pico PUBLIC NM2_HAVE_LWIP_MDNS=1)
target_compile_options(nm2_transport_lwip_pico PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nm2_transport_lwip_pico PUBLIC
    networkmidi2_pico
    pico_stdlib
    pico_lwip
    pico_lwip_nosys
    pico_lwip_mdns
)

# ---------------------------------------------------------------------------
# AM_MIDI2.0Lib (third_party/AM_MIDI2.0Lib submodule) — UMP message helpers
# (UMPMessage, umpProcessor) used by the DEVICE role's local Endpoint/
# Function Block Discovery replies (main.cpp's midiendpoint()/functionblock()).
# Unused by the HOST role but linked either way, matching ProtoZOA.
# ---------------------------------------------------------------------------
# EXCLUDE_FROM_ALL: AM_MIDI2.0Lib's own CMakeLists.txt unconditionally adds a
# libmidi2_test executable (native x86 test harness, uses <cstdarg> in a way
# that doesn't cross-compile cleanly under the ARM/newlib toolchain) — we
# only need the libmidi2 library target itself, which stays buildable as a
# dependency of nm2_bridge_pico even though it's excluded from the default
# "all" target here.
add_subdirectory(${THIRD_PARTY_DIR}/AM_MIDI2.0Lib ${CMAKE_CURRENT_BINARY_DIR}/AM_MIDI2.0Lib EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# tusb_ump — USB UMP class driver (third_party/tusb_ump submodule).
# ump_device.cpp / ump_host.cpp each self-guard on CFG_TUD_ENABLED /
# CFG_TUH_ENABLED (unset/0 in the inactive role's tusb_config.h), so globbing
# both in is safe regardless of role — only the active one compiles to
# anything.
# ---------------------------------------------------------------------------
file(GLOB TUSB_UMP_SRCS CONFIGURE_DEPENDS ${THIRD_PARTY_DIR}/tusb_ump/*.cpp)

# usb_descriptors.cpp is DEVICE-role only (references CFG_TUD_* macros that
# don't exist in the HOST role's tusb_config.h).
set(NM2_BRIDGE_SRC main.cpp bridge_config.cpp console_menu.cpp ${TUSB_UMP_SRCS})
if(NOT NM2_BRIDGE_USB_HOST)
    list(APPEND NM2_BRIDGE_SRC usb_descriptors.cpp)
endif()

# ---------------------------------------------------------------------------
# Example executable
# ---------------------------------------------------------------------------
add_executable(nm2_bridge_pico ${NM2_BRIDGE_SRC})

if(NM2_BRIDGE_USB_HOST)
    # host/tusb_config.h must be found ahead of anything else that might
    # provide a device-mode tusb_config.h.
    target_include_directories(nm2_bridge_pico PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/host
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040
    )
else()
    # device/tusb_config.h must be found ahead of host/'s copy.
    target_include_directories(nm2_bridge_pico PRIVATE ${CMAKE_CURRENT_LIST_DIR}/device)
    # Opt this target only out of device/tusb_config.h's CFG_TUD_CDC default
    # (see that file's NM2_BRIDGE_USB_CDC comment) -- examples/midi_bridge/
    # pico_w reuses the same tusb_config.h and still wants CDC on, so the
    # override belongs here rather than in the shared file.
    target_compile_definitions(nm2_bridge_pico PRIVATE NM2_BRIDGE_USB_CDC=0)
endif()

target_include_directories(nm2_bridge_pico PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${WIZNET_PORT_DIR}                # lwipopts.h
    ${THIRD_PARTY_DIR}/tusb_ump
    ${THIRD_PARTY_DIR}/AM_MIDI2.0Lib
    ${NM2_ROOT}/include
    ${NM2_ROOT}/transports/lwip
)

target_compile_definitions(nm2_bridge_pico PRIVATE
    NM2_HAVE_LWIP_MDNS=1
    NM2_BRIDGE_USB_HOST=${NM2_BRIDGE_USB_HOST}
)

target_compile_options(nm2_bridge_pico PRIVATE -Wno-narrowing -Wno-unused-variable)

target_link_libraries(nm2_bridge_pico PRIVATE
    nm2_transport_lwip_pico
    networkmidi2_pico
    pico_stdlib
    pico_stdio
    pico_unique_id
    $<IF:$<BOOL:${NM2_BRIDGE_USB_HOST}>,tinyusb_host,tinyusb_device>
    hardware_spi
    hardware_dma
    hardware_clocks
    hardware_flash
    libmidi2
    nm2_bridge_wiznet_port
    pico_lwip
    pico_lwip_nosys
    pico_lwip_mdns
)

if(NM2_BRIDGE_USB_HOST)
    # Swap in the patched hcd_rp2040.c (see tinyusb_overrides/hcd_rp2040.c's
    # header comment: reports USB_INTS_ERROR_RX_TIMEOUT_BITS as
    # XFER_RESULT_TIMEOUT instead of silently hanging the shared control
    # endpoint forever) in place of pico-sdk's stock copy, scoped to this
    # target only. HOST-role only — irrelevant to the DEVICE role.
    target_sources(nm2_bridge_pico PRIVATE tinyusb_overrides/hcd_rp2040.c)
    get_target_property(_tinyusb_host_base_srcs tinyusb_host_base INTERFACE_SOURCES)
    list(FILTER _tinyusb_host_base_srcs EXCLUDE REGEX "portable/raspberrypi/rp2040/hcd_rp2040\\.c$")
    set_target_properties(tinyusb_host_base PROPERTIES INTERFACE_SOURCES "${_tinyusb_host_base_srcs}")
endif()

# Both roles: console over UART0 only. HOST role's USB port is occupied by
# its own device enumeration and never had a CDC console option. DEVICE role
# used to also offer a CDC-ACM console (usb_descriptors.cpp) alongside the
# MIDI function via pico_enable_stdio_usb -- pulled for this release after
# tracing an RX wedge (keystrokes silently stop reaching the setup menu after
# a host-side tty reconnect, e.g. macOS's `tio`; see
# project_w5500_evb_pico_cdc_rx_wedge memory / git log "fix CDC console RX
# wedging"). The underlying fix (disabling pico_stdio_usb's background IRQ
# task + DTR-independent connect check, both no longer needed here since
# usb_descriptors.cpp no longer declares a CDC interface at all) is still in
# git history if CDC is reintroduced later with more field validation.
pico_enable_stdio_uart(nm2_bridge_pico 1)
pico_enable_stdio_usb(nm2_bridge_pico 0)

pico_add_extra_outputs(nm2_bridge_pico)
