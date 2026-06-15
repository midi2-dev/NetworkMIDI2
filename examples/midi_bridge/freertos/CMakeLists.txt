# NetworkMIDI2 — FreeRTOS + lwIP example for Pico 2 W
#
# Release build — links against pre-built libs in the _release/ directory.
# No NetworkMIDI2 source code required.
#
# Prerequisites:
#   PICO_SDK_PATH         path to Pico SDK 2.2.0   (default: ~/.pico-sdk/sdk/2.2.0)
#   PICO_BOARD            must be pico2_w           (default below)
#   FREERTOS_KERNEL_PATH  optional: path to FreeRTOS-Kernel V11+ local checkout.
#                         If unset, FetchContent downloads V11.1.0 automatically.
#
# Quick-start (from _release/ directory):
#   cmake -B build_pico/rp2350-rtos \
#         -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0 \
#         -DPICO_BOARD=pico2_w \
#         examples/midi_bridge/freertos
#   cmake --build build_pico/rp2350-rtos -j6
#
# Flash: copy build_pico/rp2350-rtos/nm2_pico_rtos.uf2 to the Pico 2 W.
# Console: screen /dev/cu.usbmodem<id> 115200

cmake_minimum_required(VERSION 3.18)

set(PICO_BOARD pico2_w CACHE STRING "Pico board target (default: pico2_w)")

set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Pico SDK root")
if(NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/.pico-sdk/sdk/2.2.0")
    message(STATUS "PICO_SDK_PATH not set — defaulting to ${PICO_SDK_PATH}")
endif()

# Release root is three levels up from this file: _release/examples/midi_bridge/freertos/
get_filename_component(NM2_RELEASE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

include(${NM2_RELEASE_ROOT}/cmake/pico_sdk_import.cmake)

project(nm2_pico_rtos CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# FreeRTOS Kernel — V11.1.0+ (RP2350 = Cortex-M33 NTZ port)
#
# freertos_config INTERFACE target must exist BEFORE FetchContent_MakeAvailable.
# ---------------------------------------------------------------------------
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)

set(FREERTOS_PORT GCC_ARM_CM33_NTZ_NONSECURE CACHE STRING "FreeRTOS port")
set(FREERTOS_HEAP 4 CACHE STRING "FreeRTOS heap implementation (heap_4)")

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
        set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
    endif()
    add_subdirectory(${FREERTOS_KERNEL_PATH} freertos_kernel)
endif()

pico_sdk_init()

# ---------------------------------------------------------------------------
# NetworkMIDI2 core — IMPORTED from pre-built release library
# ---------------------------------------------------------------------------
add_library(networkmidi2_rtos STATIC IMPORTED)
set_target_properties(networkmidi2_rtos PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/pico/rp2350-rtos/libnetworkmidi2.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/include"
)

# ---------------------------------------------------------------------------
# lwIP transport (threadsafe_background / FreeRTOS variant)
#
# IMPORTANT: ${CMAKE_CURRENT_LIST_DIR} (lwipopts.h, FreeRTOSConfig.h) MUST
# appear FIRST in the include path.  Added by the app's own
# target_include_directories below.
# ---------------------------------------------------------------------------
add_library(nm2_transport_lwip_rtos STATIC IMPORTED GLOBAL)
set_target_properties(nm2_transport_lwip_rtos PROPERTIES
    IMPORTED_LOCATION "${NM2_RELEASE_ROOT}/lib/pico/rp2350-rtos/libnm2_transport_lwip.a"
    INTERFACE_INCLUDE_DIRECTORIES "${NM2_RELEASE_ROOT}/transports/lwip;${NM2_RELEASE_ROOT}/include"
    INTERFACE_COMPILE_DEFINITIONS "NM2_HAVE_LWIP_MDNS=1"
    INTERFACE_LINK_LIBRARIES "networkmidi2_rtos;pico_stdlib;pico_cyw43_arch_lwip_threadsafe_background;pico_lwip_mdns"
)

# ---------------------------------------------------------------------------
# Example executable
# ---------------------------------------------------------------------------
add_executable(nm2_pico_rtos
    main.cpp
    SessionTask.cpp
    freertos_irq_glue.c
    ../common/DemoMidiSource.cpp
)

# lwipopts.h and FreeRTOSConfig.h MUST be the first include directory.
target_include_directories(nm2_pico_rtos PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}               # lwipopts.h, FreeRTOSConfig.h — first
    ${NM2_RELEASE_ROOT}/include
    ${NM2_RELEASE_ROOT}/transports/lwip
    ${CMAKE_CURRENT_LIST_DIR}/../common
)
target_compile_definitions(nm2_pico_rtos PRIVATE
    NM2_HAVE_LWIP_MDNS=1
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
)
target_compile_options(nm2_pico_rtos PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nm2_pico_rtos PRIVATE
    nm2_transport_lwip_rtos
    networkmidi2_rtos
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mdns
    pico_flash
    freertos_kernel
)

pico_enable_stdio_usb(nm2_pico_rtos 1)
pico_enable_stdio_uart(nm2_pico_rtos 0)
pico_add_extra_outputs(nm2_pico_rtos)
