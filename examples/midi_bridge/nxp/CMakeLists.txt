# NetworkMIDI2 — FreeRTOS + lwIP + USB MIDI 2.0 device/host example for
# NXP FRDM-MCXN947
#
# Release build — links NetworkMIDI2 core + the NXP transport against the
# pre-built libs in this distribution (lib/nxp/mcxn947/). USB MIDI support
# (TinyUSB, tusb_ump) and AM_MIDI2.0Lib's UMP helpers are built from source
# from the third_party/ git submodules below -- these are public upstream
# projects, not part of NetworkMIDI2's own proprietary source, so they are
# not pre-built.
#
# Prerequisites:
#   MCUX_SDK_PATH         path to NXP MCUXpresso SDK 2.16.x for FRDM-MCXN947
#                         (env $MCUX_SDK_PATH or -DMCUX_SDK_PATH=...)
#   ARMGCC_DIR            path to arm-none-eabi toolchain (or on PATH)
#   third_party/ submodules -- run once after cloning this release repo:
#     git submodule update --init --recursive
#
# USB role (build-time choice, matches the Pico examples):
#   -DNM2_BRIDGE_USB_ROLE=DEVICE (default) or HOST
#
# Quick-start (from _release/ directory):
#   git submodule update --init --recursive
#   cmake -B build_nxp \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-nxp-mcxn947.cmake \
#         -DMCUX_SDK_PATH=$HOME/sdk/SDK_2_16_100_FRDM-MCXN947 \
#         examples/midi_bridge/nxp
#   cmake --build build_nxp -j6
#
# Flash:
#   pyocd flash --target mcxn947 --format elf build_nxp/nm2_nxp_mcxn947
#
# Console: MCU-Link virtual COM port at 115200 baud.

cmake_minimum_required(VERSION 3.18)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/toolchain-nxp-mcxn947.cmake"
        CACHE PATH "NXP MCXN947 toolchain")
endif()

project(nm2_nxp_mcxn947 CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release root is three levels up: _release/examples/midi_bridge/nxp/
get_filename_component(NM2_RELEASE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# ---------------------------------------------------------------------------
# USB role: HOST or DEVICE (default), same toggle/rationale as
# examples/midi_bridge/pico/CMakeLists.txt. DEVICE is the already
# hardware-validated role -- kept as default so existing builds/docs that
# don't pass this option are unaffected.
# ---------------------------------------------------------------------------
set(NM2_BRIDGE_USB_ROLE "DEVICE" CACHE STRING "USB role: HOST or DEVICE")
set_property(CACHE NM2_BRIDGE_USB_ROLE PROPERTY STRINGS HOST DEVICE)
if(NM2_BRIDGE_USB_ROLE STREQUAL "HOST")
    set(NM2_BRIDGE_USB_HOST 1)
else()
    set(NM2_BRIDGE_USB_HOST 0)
endif()

# ---------------------------------------------------------------------------
# third_party/ submodules (tusb_ump, AM_MIDI2.0Lib, tinyusb) -- public
# upstream projects vendored as git submodules in this release repo, not
# part of NetworkMIDI2's own proprietary source. Verify they were cloned
# (each checked against a file that only exists once populated -- tusb_ump
# has no CMakeLists.txt of its own, it's consumed as plain sources).
# ---------------------------------------------------------------------------
foreach(_nm2_submodule_check
        "tusb_ump/ump_device.cpp"
        "AM_MIDI2.0Lib/CMakeLists.txt"
        "tinyusb/src/CMakeLists.txt")
    if(NOT EXISTS "${NM2_RELEASE_ROOT}/third_party/${_nm2_submodule_check}")
        message(FATAL_ERROR
            "third_party/ submodules are missing or empty "
            "(expected ${_nm2_submodule_check}).\n"
            "Run from the release repo root: git submodule update --init --recursive")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# NXP MCUXpresso SDK path
# ---------------------------------------------------------------------------
if(NOT MCUX_SDK_PATH)
    if(DEFINED ENV{MCUX_SDK_PATH})
        set(MCUX_SDK_PATH "$ENV{MCUX_SDK_PATH}")
    else()
        message(FATAL_ERROR "MCUX_SDK_PATH not set. Pass -DMCUX_SDK_PATH=/path/to/SDK_2_16_100_FRDM-MCXN947")
    endif()
endif()
message(STATUS "MCUXpresso SDK: ${MCUX_SDK_PATH}")

# ---------------------------------------------------------------------------
# Path variables
# ---------------------------------------------------------------------------
set(SDK_DEVICE_PATH     "${MCUX_SDK_PATH}/devices/MCXN947")
set(SDK_COMPONENTS_PATH "${MCUX_SDK_PATH}/components")
set(SDK_MIDDLEWARE_PATH "${MCUX_SDK_PATH}/middleware")
set(SDK_RTOS_PATH       "${MCUX_SDK_PATH}/rtos/freertos/freertos-kernel")
set(SDK_LWIP_PATH       "${SDK_MIDDLEWARE_PATH}/lwip")

# Board support files from the SDK's lwip_dhcp FreeRTOS example
# (contains board.c / clock_config.c / pin_mux.c for ENET + LAN8741 PHY)
set(SDK_BOARD_PATH
    "${MCUX_SDK_PATH}/boards/frdmmcxn947/lwip_examples/lwip_dhcp/freertos/cm33_core0")

# ---------------------------------------------------------------------------
# FreeRTOS Kernel — from SDK (V11.1.0 bundled)
# GCC_ARM_CM33_NTZ_NONSECURE port — MCX N947 Cortex-M33 without TrustZone
# ---------------------------------------------------------------------------
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)

set(FREERTOS_PORT GCC_ARM_CM33_NTZ_NONSECURE CACHE STRING "FreeRTOS port")
set(FREERTOS_HEAP 4                           CACHE STRING "FreeRTOS heap implementation")

add_subdirectory(${SDK_RTOS_PATH} freertos_kernel)

# ---------------------------------------------------------------------------
# NXP SDK source groups
# ---------------------------------------------------------------------------

# --- Device startup + system init ---
set(NXP_DEVICE_SRCS
    "${SDK_DEVICE_PATH}/mcuxpresso/startup_mcxn947_cm33_core0.c"
    "${SDK_DEVICE_PATH}/system_MCXN947_cm33_core0.c"
)
set(NXP_DEVICE_INCS
    "${SDK_DEVICE_PATH}"
)

# --- Board support (from lwip_dhcp FreeRTOS example — has ENET + LAN8741 config) ---
set(NXP_BOARD_SRCS
    "${SDK_BOARD_PATH}/board.c"
    "${SDK_BOARD_PATH}/clock_config.c"
    "${SDK_BOARD_PATH}/pin_mux.c"
)
set(NXP_BOARD_INCS
    "${SDK_BOARD_PATH}"
)

# --- Core drivers (all in devices/MCXN947/drivers/) ---
# MCXN947 uses MCX ENET (Synopsys DWC Ethernet, not LPC ENET or QoS variant)
set(NXP_DRIVER_SRCS
    "${SDK_DEVICE_PATH}/drivers/fsl_common.c"
    "${SDK_DEVICE_PATH}/drivers/fsl_common_arm.c"
    "${SDK_DEVICE_PATH}/drivers/fsl_clock.c"
    "${SDK_DEVICE_PATH}/drivers/fsl_enet.c"
    "${SDK_DEVICE_PATH}/drivers/fsl_lpuart.c"
    "${SDK_DEVICE_PATH}/drivers/fsl_lpflexcomm.c"
    "${SDK_DEVICE_PATH}/drivers/fsl_gpio.c"
    "${SDK_DEVICE_PATH}/drivers/fsl_reset.c"
    "${SDK_DEVICE_PATH}/drivers/fsl_spc.c"
)
set(NXP_DRIVER_INCS
    "${SDK_DEVICE_PATH}/drivers"
)

# --- PHY component: LAN8741 (on FRDM-MCXN947 Ethernet port) ---
set(NXP_PHY_SRCS
    "${SDK_COMPONENTS_PATH}/phy/device/phylan8741/fsl_phylan8741.c"
)
set(NXP_PHY_INCS
    "${SDK_COMPONENTS_PATH}/phy"
    "${SDK_COMPONENTS_PATH}/phy/device/phylan8741"
)

# --- GPIO adapter (needed by ethernetif_priv.h) ---
set(NXP_GPIO_ADAPTER_SRCS
    "${SDK_COMPONENTS_PATH}/gpio/fsl_adapter_gpio.c"
)
set(NXP_GPIO_ADAPTER_INCS
    "${SDK_COMPONENTS_PATH}/gpio"
)

# --- UART adapter + debug console lite (no serial-manager dependencies) ---
set(NXP_DBG_SRCS
    "${SDK_COMPONENTS_PATH}/uart/fsl_adapter_lpuart.c"
    "${SDK_DEVICE_PATH}/utilities/debug_console_lite/fsl_debug_console.c"
    "${SDK_DEVICE_PATH}/utilities/str/fsl_str.c"
)
set(NXP_DBG_INCS
    "${SDK_COMPONENTS_PATH}/uart"
    "${SDK_DEVICE_PATH}/utilities/debug_console_lite"
    "${SDK_DEVICE_PATH}/utilities/str"
)

# --- lwIP sys_arch (FreeRTOS dynamic allocation) ---
set(NXP_SYSARCH_SRCS
    "${SDK_LWIP_PATH}/port/sys_arch/dynamic/sys_arch.c"
)
set(NXP_SYSARCH_INCS
    "${SDK_LWIP_PATH}/port/sys_arch/dynamic"
    "${SDK_LWIP_PATH}/port"
    "${SDK_LWIP_PATH}/port/arch"
)

# --- lwIP MCX Ethernet netif ---
# enet_ethernetif_lpc.c provides ethernetif0_init / ethernetif1_init for MCX ENET.
set(NXP_ETHERNETIF_SRCS
    "${SDK_LWIP_PATH}/port/ethernetif.c"
    "${SDK_LWIP_PATH}/port/enet_ethernetif.c"
    "${SDK_LWIP_PATH}/port/enet_ethernetif_lpc.c"
    "${SDK_LWIP_PATH}/port/ethernetif_mmac.c"
)

# ---------------------------------------------------------------------------
# Aggregate include list (used by multiple targets)
# ---------------------------------------------------------------------------
set(NXP_ALL_INCS
    ${CMAKE_CURRENT_LIST_DIR}       # lwipopts.h, FreeRTOSConfig.h — MUST be first
    ${NXP_DEVICE_INCS}
    ${NXP_BOARD_INCS}
    ${NXP_DRIVER_INCS}
    ${NXP_PHY_INCS}
    ${NXP_GPIO_ADAPTER_INCS}
    ${NXP_DBG_INCS}
    ${NXP_SYSARCH_INCS}
    "${SDK_LWIP_PATH}/src/include"
    "${MCUX_SDK_PATH}/CMSIS/Core/Include"
)

# ---------------------------------------------------------------------------
# lwIP core library
# Built from SDK sources so lwipopts.h from CMAKE_CURRENT_LIST_DIR takes precedence.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE LWIP_CORE_SRCS
    "${SDK_LWIP_PATH}/src/core/*.c"
    "${SDK_LWIP_PATH}/src/core/ipv4/*.c"
    "${SDK_LWIP_PATH}/src/api/*.c"
    "${SDK_LWIP_PATH}/src/apps/mdns/*.c"
    "${SDK_LWIP_PATH}/src/netif/ethernet.c"
)

add_library(lwip_nxp STATIC
    ${LWIP_CORE_SRCS}
    ${NXP_SYSARCH_SRCS}
    ${NXP_ETHERNETIF_SRCS}
    ${NXP_PHY_SRCS}
    ${NXP_GPIO_ADAPTER_SRCS}
)
target_include_directories(lwip_nxp PUBLIC ${NXP_ALL_INCS})
target_compile_definitions(lwip_nxp PUBLIC
    CPU_MCXN947VDF_cm33_core0
    MCUXPRESSO_SDK
    SDK_OS_FREE_RTOS
    USE_RTOS=1
    LWIP_DISABLE_PBUF_POOL_SIZE_SANITY_CHECKS=1
    SDK_DEBUGCONSOLE=1
    # Hardware checksum offload (required by enet_configchecks.h — all must match)
    CHECKSUM_GEN_UDP=1
    CHECKSUM_GEN_TCP=1
    CHECKSUM_GEN_ICMP=1
    CHECKSUM_GEN_ICMP6=1
    CHECKSUM_GEN_IP=1
    CHECKSUM_CHECK_IP=1
    CHECKSUM_CHECK_UDP=1
    CHECKSUM_CHECK_TCP=1
    CHECKSUM_CHECK_ICMP=1
    CHECKSUM_CHECK_ICMP6=1
)
target_link_libraries(lwip_nxp PUBLIC freertos_kernel)

# ---------------------------------------------------------------------------
# NetworkMIDI2 core — IMPORTED from pre-built release library
# ---------------------------------------------------------------------------
add_library(networkmidi2 STATIC IMPORTED)
set_target_properties(networkmidi2 PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/nxp/mcxn947/libnetworkmidi2.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/include"
)

# ---------------------------------------------------------------------------
# AM_MIDI2.0Lib (third_party/AM_MIDI2.0Lib submodule) -- UMP message helpers
# (UMPMessage, umpProcessor) used for local Endpoint/Function Block Discovery
# replies in SessionTask.cpp's midiendpoint()/functionblock(), same as the
# Pico DEVICE-role build.
#
# EXCLUDE_FROM_ALL: AM_MIDI2.0Lib's own CMakeLists.txt unconditionally adds a
# libmidi2_test executable (native x86 test harness, uses <cstdarg> in a way
# that doesn't cross-compile cleanly under the ARM/newlib toolchain) -- we
# only need the libmidi2 library target itself.
# ---------------------------------------------------------------------------
add_subdirectory(${NM2_RELEASE_ROOT}/third_party/AM_MIDI2.0Lib ${CMAKE_CURRENT_BINARY_DIR}/AM_MIDI2.0Lib EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# TinyUSB device/host stack (USB1 ChipIdea High-Speed) + tusb_ump UMP class
# driver
#
# MCXN947's USB1 HS controller uses TinyUSB's "chipidea/ci_hs" portable
# driver (CFG_TUSB_MCU=OPT_MCU_MCXN9). Only the portable driver (dcd_ci_hs.c
# for DEVICE role, register-level only; tinyusb_overrides/hcd_ci_hs.c for
# HOST role, see its own header comment) and TinyUSB's own core/device/host
# sources are used here -- NOT TinyUSB's own board.c/pin_mux.c/clock_config.c,
# which would collide with this example's own board init (see
# NM2_UsbHsInit() in board_init.cpp).
# ---------------------------------------------------------------------------
set(TINYUSB_ROOT ${NM2_RELEASE_ROOT}/third_party/tinyusb)

set(CFG_TUSB_DEBUG "0" CACHE STRING "TinyUSB debug log level (0-3) -- verbose device-stack logging over the LPUART console")

add_library(nm2_nxp_tinyusb_config INTERFACE)
if(NM2_BRIDGE_USB_HOST)
    # host/tusb_config.h must be found ahead of device/'s copy.
    target_include_directories(nm2_nxp_tinyusb_config INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/host
    )
    target_compile_definitions(nm2_nxp_tinyusb_config INTERFACE
        BOARD_TUH_RHPORT=1
        BOARD_TUH_MAX_SPEED=OPT_MODE_HIGH_SPEED
    )
else()
    # device/tusb_config.h must be found ahead of host/'s copy.
    target_include_directories(nm2_nxp_tinyusb_config INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/device
    )
    target_compile_definitions(nm2_nxp_tinyusb_config INTERFACE
        BOARD_TUD_RHPORT=1
        BOARD_TUD_MAX_SPEED=OPT_MODE_HIGH_SPEED
    )
endif()
target_compile_definitions(nm2_nxp_tinyusb_config INTERFACE
    CFG_TUSB_MCU=OPT_MCU_MCXN9
    CFG_TUSB_OS=OPT_OS_FREERTOS
    CFG_TUSB_DEBUG=${CFG_TUSB_DEBUG}
)
target_link_libraries(nm2_nxp_tinyusb_config INTERFACE freertos_config)

set(TINYUSB_TARGET_PREFIX "nm2_nxp_")
add_subdirectory(${TINYUSB_ROOT}/src ${CMAKE_CURRENT_BINARY_DIR}/tinyusb)

# Portable driver: not added by tinyusb's own src/CMakeLists.txt (that's left
# to board-level glue in stock TinyUSB examples) -- add it explicitly here.
# DEVICE role needs the device controller driver (dcd); HOST role needs the
# host controller driver (hcd) instead -- both live in the same ci_hs/
# directory, mutually exclusive per role.
if(NM2_BRIDGE_USB_HOST)
    # tinyusb_overrides/hcd_ci_hs.c: a locally patched copy of the stock
    # submodule file, adding the OPT_MCU_MCXN9 branch that only its
    # DEVICE-role sibling (dcd_ci_hs.c) has upstream -- see that file's
    # header comment for the full rationale. Its quoted #includes
    # (ci_hs_type.h, ci_hs_mcx.h, ...) resolve relative to dcd_ci_hs.c's own
    # directory when compiled there, but this override lives elsewhere, so
    # that directory must be added to the include path explicitly.
    # Chipidea HS's host driver is a thin register-level shim over TinyUSB's
    # shared generic EHCI core implementation (portable/ehci/ehci.c) --
    # add_tinyusb() (src/CMakeLists.txt) doesn't include it since not every
    # host controller uses EHCI, same reason ci_hs' own device counterpart
    # (dcd_ci_hs.c) needs adding explicitly above/below.
    target_sources(nm2_nxp_tinyusb PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/tinyusb_overrides/hcd_ci_hs.c
        ${TINYUSB_ROOT}/src/portable/ehci/ehci.c
    )
    target_include_directories(nm2_nxp_tinyusb PUBLIC
        ${TINYUSB_ROOT}/src/portable/chipidea/ci_hs
    )
else()
    target_sources(nm2_nxp_tinyusb PRIVATE
        ${TINYUSB_ROOT}/src/portable/chipidea/ci_hs/dcd_ci_hs.c
    )
endif()
target_include_directories(nm2_nxp_tinyusb PUBLIC
    ${NXP_ALL_INCS}   # fsl_device_registers.h, CMSIS Core, etc. (device-register-level only)
)
target_compile_definitions(nm2_nxp_tinyusb PUBLIC
    CPU_MCXN947VDF_cm33_core0
)
target_link_libraries(nm2_nxp_tinyusb PUBLIC freertos_kernel)

# TinyUSB's own CMakeLists.txt (add_tinyusb()) builds every source in this
# target with -Werror, which turns the NXP SDK's harmless SystemCoreClock
# redundant-extern-declaration (system_MCXN947_cm33_core0.h vs. a second
# declaration pulled in transitively via fsl_device_registers.h, reached only
# through dcd_ci_hs.c's ci_hs_mcx.h include) into a hard build failure.
# Cancel -Werror for this target (added last so it wins on the command line) --
# this is downstream integration, not TinyUSB's own CI, so its stricter
# warnings-as-errors policy doesn't need to apply here.
target_compile_options(nm2_nxp_tinyusb PRIVATE -Wno-error)

# ---------------------------------------------------------------------------
# NXP UDP transport + mDNS — IMPORTED from pre-built release library
# ---------------------------------------------------------------------------
add_library(nm2_transport_nxp STATIC IMPORTED GLOBAL)
set_target_properties(nm2_transport_nxp PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/nxp/mcxn947/libnm2_transport_nxp.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/transports/nxp;${NM2_RELEASE_ROOT}/include"
    INTERFACE_COMPILE_DEFINITIONS "NM2_HAVE_LWIP_MDNS=1"
    INTERFACE_LINK_LIBRARIES "networkmidi2;lwip_nxp"
)

# ---------------------------------------------------------------------------
# Example executable
#
# usb_descriptors.cpp is DEVICE-role only (references CFG_TUD_* macros that
# don't exist in the HOST role's tusb_config.h); ump_device.cpp/ump_host.cpp
# each self-guard on CFG_TUD_ENABLED/CFG_TUH_ENABLED, so only the active
# role's file is needed.
# ---------------------------------------------------------------------------
set(NM2_BRIDGE_USB_SRC main.cpp SessionTask.cpp board_init.cpp)
if(NM2_BRIDGE_USB_HOST)
    list(APPEND NM2_BRIDGE_USB_SRC ${NM2_RELEASE_ROOT}/third_party/tusb_ump/ump_host.cpp)
else()
    list(APPEND NM2_BRIDGE_USB_SRC usb_descriptors.cpp ${NM2_RELEASE_ROOT}/third_party/tusb_ump/ump_device.cpp)
endif()

add_executable(nm2_nxp_mcxn947
    ${NM2_BRIDGE_USB_SRC}
    ${NXP_DEVICE_SRCS}
    ${NXP_BOARD_SRCS}
    ${NXP_DRIVER_SRCS}
    ${NXP_DBG_SRCS}
)

target_include_directories(nm2_nxp_mcxn947 PRIVATE
    ${NXP_ALL_INCS}
    ${NM2_RELEASE_ROOT}/include
    ${NM2_RELEASE_ROOT}/transports/nxp
    ${NM2_RELEASE_ROOT}/examples/midi_bridge/freertos   # DroppingTransport.h (portable)
    ${NM2_RELEASE_ROOT}/third_party/tusb_ump
    ${NM2_RELEASE_ROOT}/third_party/AM_MIDI2.0Lib
)

target_compile_definitions(nm2_nxp_mcxn947 PRIVATE
    NM2_HAVE_LWIP_MDNS=1
    NM2_BRIDGE_USB_HOST=${NM2_BRIDGE_USB_HOST}
    CPU_MCXN947VDF_cm33_core0
    MCUXPRESSO_SDK
    SDK_OS_FREE_RTOS
    USE_RTOS=1
    SDK_DEBUGCONSOLE=1
    SERIAL_PORT_TYPE_UART=1
    # Suppress floating-point in printf (saves ~5 KB)
    PRINTF_FLOAT_ENABLE=0
    CR_INTEGER_PRINTF
    # Required: causes startup to call SystemInit() which enables the FPU,
    # disables glitch-detect chip-reset, and enables flash cache.
    __USE_CMSIS
)

target_compile_options(nm2_nxp_mcxn947 PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    # Suppress warning about startup.c CRT initialisation pattern
    -Wno-missing-declarations
)

target_link_libraries(nm2_nxp_mcxn947 PRIVATE
    nm2_transport_nxp
    networkmidi2
    lwip_nxp
    freertos_kernel
    nm2_nxp_tinyusb
    libmidi2
)

# ---------------------------------------------------------------------------
# Linker script
# Workspace-generated MCUXpresso linker scripts (linker/ subdirectory).
# These define flash and SRAM regions for MCX N947/N547 Cortex-M33 core 0.
# ---------------------------------------------------------------------------
set(NXP_LINKER_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/linker/nm2_nxp_mcxn947.ld")
if(EXISTS "${NXP_LINKER_SCRIPT}")
    target_link_options(nm2_nxp_mcxn947 PRIVATE
        "-T${NXP_LINKER_SCRIPT}"
        "-L${CMAKE_CURRENT_LIST_DIR}/linker"   # for INCLUDE "..." in the .ld
        "-Wl,-Map=nm2_nxp_mcxn947.map"
        "-Wl,--gc-sections"
        "-Wl,-no-warn-rwx-segments"
        -nostdlib
    )
else()
    message(WARNING "Linker script not found: ${NXP_LINKER_SCRIPT}")
endif()

# ---------------------------------------------------------------------------
# Post-build: generate .bin and .hex alongside the .elf
# ---------------------------------------------------------------------------
find_program(NXP_OBJCOPY arm-none-eabi-objcopy)
if(NXP_OBJCOPY)
    add_custom_command(TARGET nm2_nxp_mcxn947 POST_BUILD
        COMMAND ${NXP_OBJCOPY} -O binary
            $<TARGET_FILE:nm2_nxp_mcxn947>
            $<TARGET_FILE_DIR:nm2_nxp_mcxn947>/nm2_nxp_mcxn947.bin
        COMMAND ${NXP_OBJCOPY} -O ihex
            $<TARGET_FILE:nm2_nxp_mcxn947>
            $<TARGET_FILE_DIR:nm2_nxp_mcxn947>/nm2_nxp_mcxn947.hex
        COMMENT "Generating nm2_nxp_mcxn947.bin and .hex"
    )
endif()
