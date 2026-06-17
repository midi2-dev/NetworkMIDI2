# NetworkMIDI2 — FreeRTOS + lwIP example for NXP FRDM-MCXN947
#
# Release build — links against pre-built libs in the _release/ directory.
# No NetworkMIDI2 source code required.
#
# Prerequisites:
#   MCUX_SDK_PATH         path to NXP MCUXpresso SDK 2.16.x root
#                         (env $MCUX_SDK_PATH or -DMCUX_SDK_PATH=...)
#   ARMGCC_DIR            path to arm-none-eabi toolchain (or on PATH)
#   FREERTOS_KERNEL_PATH  optional: local FreeRTOS-Kernel V11+ checkout.
#                         If unset, FetchContent downloads V11.1.0 automatically.
#
# Quick-start (from _release/ directory):
#   cmake -B build_nxp \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-nxp-mcxn947.cmake \
#         -DMCUX_SDK_PATH=$HOME/sdk/mcuxsdk-mcxn947 \
#         examples/midi_bridge/nxp
#   cmake --build build_nxp -j6
#
# Flash:
#   pyocd flash --target mcxn947 build_nxp/nm2_nxp_mcxn947.elf
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

# ---------------------------------------------------------------------------
# NXP MCUXpresso SDK path
# ---------------------------------------------------------------------------
if(NOT MCUX_SDK_PATH)
    if(DEFINED ENV{MCUX_SDK_PATH})
        set(MCUX_SDK_PATH "$ENV{MCUX_SDK_PATH}")
    else()
        message(FATAL_ERROR "MCUX_SDK_PATH not set. Pass -DMCUX_SDK_PATH=/path/to/sdk")
    endif()
endif()
message(STATUS "MCUXpresso SDK: ${MCUX_SDK_PATH}")

# Release root is three levels up: _release/examples/midi_bridge/nxp/
get_filename_component(NM2_RELEASE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# ---------------------------------------------------------------------------
# FreeRTOS Kernel
# ---------------------------------------------------------------------------
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)

set(FREERTOS_PORT GCC_ARM_CM33_NTZ_NONSECURE CACHE STRING "FreeRTOS port")
set(FREERTOS_HEAP 4                           CACHE STRING "FreeRTOS heap")

if(NOT DEFINED ENV{FREERTOS_KERNEL_PATH} AND NOT FREERTOS_KERNEL_PATH)
    message(STATUS "FREERTOS_KERNEL_PATH not set — fetching FreeRTOS-Kernel V11.1.0")
    include(FetchContent)
    FetchContent_Declare(freertos_kernel
        GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
        GIT_TAG        V11.1.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(freertos_kernel)
else()
    if(DEFINED ENV{FREERTOS_KERNEL_PATH} AND NOT FREERTOS_KERNEL_PATH)
        set(FREERTOS_KERNEL_PATH "$ENV{FREERTOS_KERNEL_PATH}")
    endif()
    add_subdirectory(${FREERTOS_KERNEL_PATH} freertos_kernel)
endif()

# ---------------------------------------------------------------------------
# NXP SDK source groups (same as dev build)
# ---------------------------------------------------------------------------
set(SDK_DEVICE_PATH     "${MCUX_SDK_PATH}/devices/MCXN947")
set(SDK_BOARD_PATH      "${MCUX_SDK_PATH}/boards/frdmmcxn947")
set(SDK_DRIVER_PATH     "${MCUX_SDK_PATH}/drivers")
set(SDK_MIDDLEWARE_PATH "${MCUX_SDK_PATH}/middleware")
set(SDK_UTIL_PATH       "${MCUX_SDK_PATH}/utilities")
set(SDK_LWIP_PATH       "${SDK_MIDDLEWARE_PATH}/lwip")

set(NXP_DEVICE_SRCS
    "${SDK_DEVICE_PATH}/gcc/startup_mcxn947_cm33_core0.S"
    "${SDK_DEVICE_PATH}/system_MCXN947_cm33_core0.c"
)
set(NXP_BOARD_SRCS
    "${SDK_BOARD_PATH}/board.c"
    "${SDK_BOARD_PATH}/clock_config.c"
    "${SDK_BOARD_PATH}/pin_mux.c"
)
set(NXP_DRIVER_SRCS
    "${SDK_DRIVER_PATH}/common/fsl_common.c"
    "${SDK_DRIVER_PATH}/common/fsl_common_arm.c"
    "${SDK_DRIVER_PATH}/clock/fsl_clock.c"
    "${SDK_DRIVER_PATH}/enet_qos/fsl_enet_qos.c"
    "${SDK_DRIVER_PATH}/lpuart/fsl_lpuart.c"
    "${SDK_DRIVER_PATH}/gpio/fsl_gpio.c"
    "${SDK_DRIVER_PATH}/port/fsl_port.c"
    "${SDK_DRIVER_PATH}/mdio/enet_qos/fsl_enet_qos_mdio.c"
    "${SDK_DRIVER_PATH}/phy/device/phyksz8081/fsl_phyksz8081.c"
)
set(NXP_DBG_SRCS
    "${SDK_UTIL_PATH}/debug_console/fsl_debug_console.c"
    "${SDK_UTIL_PATH}/str/fsl_str.c"
)
set(NXP_LWIP_SYSARCH_SRCS
    "${SDK_LWIP_PATH}/port/sys_arch.c"
    "${SDK_LWIP_PATH}/port/enet_ethernetif.c"
)

set(NXP_ALL_INCS
    ${CMAKE_CURRENT_LIST_DIR}
    "${NM2_RELEASE_ROOT}/include"
    "${NM2_RELEASE_ROOT}/transports/nxp"
    "${SDK_DEVICE_PATH}"
    "${SDK_DEVICE_PATH}/drivers"
    "${SDK_BOARD_PATH}"
    "${SDK_DRIVER_PATH}/common"
    "${SDK_DRIVER_PATH}/clock"
    "${SDK_DRIVER_PATH}/enet_qos"
    "${SDK_DRIVER_PATH}/lpuart"
    "${SDK_DRIVER_PATH}/gpio"
    "${SDK_DRIVER_PATH}/port"
    "${SDK_DRIVER_PATH}/mdio"
    "${SDK_DRIVER_PATH}/mdio/enet_qos"
    "${SDK_DRIVER_PATH}/phy/device/phyksz8081"
    "${SDK_DRIVER_PATH}/phy"
    "${SDK_UTIL_PATH}/debug_console"
    "${SDK_UTIL_PATH}/str"
    "${SDK_LWIP_PATH}/src/include"
    "${SDK_LWIP_PATH}/src/include/ipv4"
    "${SDK_LWIP_PATH}/port"
    "${SDK_LWIP_PATH}/port/freertos"
)

# ---------------------------------------------------------------------------
# lwIP core (built from SDK sources so our lwipopts.h takes precedence)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE LWIP_CORE_SRCS
    "${SDK_LWIP_PATH}/src/core/*.c"
    "${SDK_LWIP_PATH}/src/core/ipv4/*.c"
    "${SDK_LWIP_PATH}/src/api/*.c"
    "${SDK_LWIP_PATH}/src/apps/mdns/*.c"
)

add_library(lwip_nxp STATIC ${LWIP_CORE_SRCS} ${NXP_LWIP_SYSARCH_SRCS})
target_include_directories(lwip_nxp PUBLIC ${NXP_ALL_INCS})
target_compile_definitions(lwip_nxp PUBLIC CPU_MCXN947VDF_cm33_core0 MCUX_SDK)
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
# NXP transport + mDNS — IMPORTED from pre-built release library
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
# ---------------------------------------------------------------------------
add_executable(nm2_nxp_mcxn947
    main.cpp
    SessionTask.cpp
    board_init.cpp
    ${NXP_DEVICE_SRCS}
    ${NXP_BOARD_SRCS}
    ${NXP_DRIVER_SRCS}
    ${NXP_DBG_SRCS}
    ../common/DemoMidiSource.cpp
)

target_include_directories(nm2_nxp_mcxn947 PRIVATE ${NXP_ALL_INCS}
    ${CMAKE_CURRENT_LIST_DIR}/../common)

target_compile_definitions(nm2_nxp_mcxn947 PRIVATE
    NM2_HAVE_LWIP_MDNS=1
    CPU_MCXN947VDF_cm33_core0
    MCUX_SDK
    SDK_DEBUGCONSOLE=1
    SERIAL_PORT_TYPE_UART=1
)

target_compile_options(nm2_nxp_mcxn947 PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(nm2_nxp_mcxn947 PRIVATE
    nm2_transport_nxp
    networkmidi2
    lwip_nxp
    freertos_kernel
)

set(NXP_LINKER_SCRIPT
    "${SDK_DEVICE_PATH}/gcc/MCXN947_cm33_core0_flash.ld")
if(EXISTS "${NXP_LINKER_SCRIPT}")
    target_link_options(nm2_nxp_mcxn947 PRIVATE
        "-T${NXP_LINKER_SCRIPT}"
        "-Wl,-Map=nm2_nxp_mcxn947.map")
endif()

find_program(NXP_OBJCOPY arm-none-eabi-objcopy)
if(NXP_OBJCOPY)
    add_custom_command(TARGET nm2_nxp_mcxn947 POST_BUILD
        COMMAND ${NXP_OBJCOPY} -O binary
            $<TARGET_FILE:nm2_nxp_mcxn947>
            $<TARGET_FILE_DIR:nm2_nxp_mcxn947>/nm2_nxp_mcxn947.bin
        COMMAND ${NXP_OBJCOPY} -O ihex
            $<TARGET_FILE:nm2_nxp_mcxn947>
            $<TARGET_FILE_DIR:nm2_nxp_mcxn947>/nm2_nxp_mcxn947.hex
        COMMENT "Generating nm2_nxp_mcxn947.bin and .hex")
endif()
