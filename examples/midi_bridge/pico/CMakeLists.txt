# NetworkMIDI2 — Pico USB Bridge (RP2040 + WIZnet W5500 Ethernet)
#
# Release build — links NetworkMIDI2 core + the lwIP transport against the
# pre-built libs in this distribution (lib/pico/rp2040-w5500/ -- NOT
# lib/pico/rp2040, see that library's own comment: this target's
# wiznet_port/lwipopts.h differs in ABI-relevant ways from the plain lwip
# example's). The WIZnet SPI/lwIP-netif glue (wiznet_port/) and third_party
# USB MIDI dependencies (tusb_ump, AM_MIDI2.0Lib) are built from source --
# see "third_party dependencies" in RELEASE_NOTES.md.
#
# Two USB roles, selected at build time via NM2_BRIDGE_USB_ROLE (HOST or
# DEVICE) -- see examples/midi_bridge/pico/README.md. Two W5500 wirings via
# NM2_WIZNET_BOARD (PROTOZOA or W5500_EVB_PICO); only W5500_EVB_PICO
# (WIZnet's own eval board) is relevant outside AmeNote's own hardware.
#
# Prerequisites:
#   PICO_SDK_PATH   path to Pico SDK 2.3.0   (default: ~/.pico-sdk/sdk/2.3.0)
#   PICO_BOARD      plain "pico" (RP2040)     (default below)
#   third_party/ submodules -- run once after cloning this release repo:
#     git submodule update --init --recursive
#
# Quick-start (from _release/ directory, HOST role, W5500-EVB-Pico wiring):
#   git submodule update --init --recursive
#   cmake -B build_pico_host_evb \
#         -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
#         -DPICO_BOARD=pico \
#         -DNM2_BRIDGE_USB_ROLE=HOST \
#         -DNM2_WIZNET_BOARD=W5500_EVB_PICO \
#         examples/midi_bridge/pico
#   cmake --build build_pico_host_evb -j6
#
# Flash: copy build_pico_host_evb/nm2_bridge_pico.uf2 to the Pico held in
# BOOTSEL mode. Console: UART0 only, both roles -- see README.md.

cmake_minimum_required(VERSION 3.18)

set(PICO_BOARD pico CACHE STRING "Pico board target (default: pico, RP2040)")

set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Pico SDK root")
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.3.0")
    message(STATUS "PICO_SDK_PATH not set — defaulting to ${PICO_SDK_PATH}")
endif()

# Release root is three levels up: _release/examples/midi_bridge/pico/
get_filename_component(NM2_RELEASE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

include(${NM2_RELEASE_ROOT}/cmake/pico_sdk_import.cmake)

project(nm2_bridge_pico CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

pico_sdk_init()

set(WIZNET_PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/wiznet_port)

# ---------------------------------------------------------------------------
# third_party/ submodules (tusb_ump, AM_MIDI2.0Lib) -- public upstream
# projects vendored as git submodules in this release repo, not part of
# NetworkMIDI2's own proprietary source. Verify they were cloned.
# ---------------------------------------------------------------------------
foreach(_nm2_submodule_check
        "tusb_ump/ump_device.cpp"
        "AM_MIDI2.0Lib/CMakeLists.txt"
        "ioLibrary_Driver/Ethernet/socket.c")
    if(NOT EXISTS "${NM2_RELEASE_ROOT}/third_party/${_nm2_submodule_check}")
        message(FATAL_ERROR
            "third_party/ submodules are missing or empty "
            "(expected ${_nm2_submodule_check}).\n"
            "Run from the release repo root: git submodule update --init --recursive")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# NM2_WIZNET_BOARD / NM2_BRIDGE_USB_ROLE options -- same as the dev repo.
# ---------------------------------------------------------------------------
set(NM2_WIZNET_BOARD "PROTOZOA" CACHE STRING "Wiznet SPI pin set: PROTOZOA or W5500_EVB_PICO")
set_property(CACHE NM2_WIZNET_BOARD PROPERTY STRINGS PROTOZOA W5500_EVB_PICO)

set(NM2_BRIDGE_USB_ROLE "HOST" CACHE STRING "USB role: HOST or DEVICE")
set_property(CACHE NM2_BRIDGE_USB_ROLE PROPERTY STRINGS HOST DEVICE)
if(NM2_BRIDGE_USB_ROLE STREQUAL "HOST")
    set(NM2_BRIDGE_USB_HOST 1)
else()
    set(NM2_BRIDGE_USB_HOST 0)
endif()

# ---------------------------------------------------------------------------
# WIZnet ioLibrary_Driver (third_party/ioLibrary_Driver submodule) — W5500.
# ---------------------------------------------------------------------------
set(WIZNET_DIR ${NM2_RELEASE_ROOT}/third_party/ioLibrary_Driver)

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

target_include_directories(pico_lwip INTERFACE ${WIZNET_PORT_DIR})

# ---------------------------------------------------------------------------
# NetworkMIDI2 core — IMPORTED from pre-built release library
# ---------------------------------------------------------------------------
add_library(networkmidi2 STATIC IMPORTED)
set_target_properties(networkmidi2 PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/pico/rp2040-w5500/libnetworkmidi2.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/include"
)

# ---------------------------------------------------------------------------
# lwIP transport — IMPORTED from pre-built release library. This is the
# rp2040-w5500 variant, compiled against wiznet_port/lwipopts.h -- do NOT
# substitute lib/pico/rp2040 here (different lwipopts.h, ABI-incompatible).
# ---------------------------------------------------------------------------
add_library(nm2_transport_lwip STATIC IMPORTED GLOBAL)
set_target_properties(nm2_transport_lwip PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/pico/rp2040-w5500/libnm2_transport_lwip.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/transports/lwip;${NM2_RELEASE_ROOT}/include"
    INTERFACE_COMPILE_DEFINITIONS "NM2_HAVE_LWIP_MDNS=1"
    INTERFACE_LINK_LIBRARIES "networkmidi2;pico_stdlib;hardware_spi;hardware_dma;hardware_clocks;hardware_flash;nm2_bridge_wiznet_port;pico_lwip;pico_lwip_nosys;pico_lwip_mdns"
)

# ---------------------------------------------------------------------------
# AM_MIDI2.0Lib (third_party/AM_MIDI2.0Lib submodule)
# ---------------------------------------------------------------------------
add_subdirectory(${NM2_RELEASE_ROOT}/third_party/AM_MIDI2.0Lib ${CMAKE_CURRENT_BINARY_DIR}/AM_MIDI2.0Lib EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# tusb_ump — USB UMP class driver (third_party/tusb_ump submodule).
# ump_device.cpp/ump_host.cpp each self-guard on CFG_TUD_ENABLED/
# CFG_TUH_ENABLED, so globbing both in is safe regardless of role.
# ---------------------------------------------------------------------------
file(GLOB TUSB_UMP_SRCS CONFIGURE_DEPENDS ${NM2_RELEASE_ROOT}/third_party/tusb_ump/*.cpp)

# usb_descriptors.cpp is DEVICE-role only.
set(NM2_BRIDGE_SRC main.cpp bridge_config.cpp console_menu.cpp ${TUSB_UMP_SRCS})
if(NOT NM2_BRIDGE_USB_HOST)
    list(APPEND NM2_BRIDGE_SRC usb_descriptors.cpp)
endif()

# ---------------------------------------------------------------------------
# Example executable
# ---------------------------------------------------------------------------
add_executable(nm2_bridge_pico ${NM2_BRIDGE_SRC})

if(NM2_BRIDGE_USB_HOST)
    target_include_directories(nm2_bridge_pico PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/host
        ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040
    )
else()
    target_include_directories(nm2_bridge_pico PRIVATE ${CMAKE_CURRENT_LIST_DIR}/device)
    target_compile_definitions(nm2_bridge_pico PRIVATE NM2_BRIDGE_USB_CDC=0)
endif()

target_include_directories(nm2_bridge_pico PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}           # lwipopts.h — MUST be first, matches nm2_transport_lwip's own lwipopts.h
    ${WIZNET_PORT_DIR}
    ${NM2_RELEASE_ROOT}/third_party/tusb_ump
    ${NM2_RELEASE_ROOT}/third_party/AM_MIDI2.0Lib
    ${NM2_RELEASE_ROOT}/include
    ${NM2_RELEASE_ROOT}/transports/lwip
)

target_compile_definitions(nm2_bridge_pico PRIVATE
    NM2_HAVE_LWIP_MDNS=1
    NM2_BRIDGE_USB_HOST=${NM2_BRIDGE_USB_HOST}
)

target_compile_options(nm2_bridge_pico PRIVATE -Wno-narrowing -Wno-unused-variable)

target_link_libraries(nm2_bridge_pico PRIVATE
    nm2_transport_lwip
    networkmidi2
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
    # Swap in the patched hcd_rp2040.c -- see that file's own header comment.
    target_sources(nm2_bridge_pico PRIVATE tinyusb_overrides/hcd_rp2040.c)
    get_target_property(_tinyusb_host_base_srcs tinyusb_host_base INTERFACE_SOURCES)
    list(FILTER _tinyusb_host_base_srcs EXCLUDE REGEX "portable/raspberrypi/rp2040/hcd_rp2040\\.c$")
    set_target_properties(tinyusb_host_base PROPERTIES INTERFACE_SOURCES "${_tinyusb_host_base_srcs}")
endif()

pico_enable_stdio_uart(nm2_bridge_pico 1)
pico_enable_stdio_usb(nm2_bridge_pico 0)

pico_add_extra_outputs(nm2_bridge_pico)
