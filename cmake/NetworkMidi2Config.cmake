# NetworkMidi2Config.cmake
# CMake find_package() support for the AmeNote NetworkMIDI2 binary release.
#
# Usage in your CMakeLists.txt:
#
#   find_package(NetworkMidi2 REQUIRED)
#   target_link_libraries(my_app PRIVATE NetworkMidi2::networkmidi2)
#   # or for POSIX desktop:
#   target_link_libraries(my_app PRIVATE NetworkMidi2::nm2_transport_posix)
#   # or for lwIP (Pico/embedded):
#   target_link_libraries(my_app PRIVATE NetworkMidi2::nm2_transport_lwip)
#
# Set NM2_ROOT to the directory containing this file if find_package cannot
# locate it automatically.

cmake_minimum_required(VERSION 3.14)

get_filename_component(_NM2_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# ---------------------------------------------------------------------------
# Platform / arch detection
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64"
       OR CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(_NM2_LIB_DIR "${_NM2_ROOT}/lib/macos/x86_64")
    else()
        set(_NM2_LIB_DIR "${_NM2_ROOT}/lib/macos/arm64")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
        set(_NM2_LIB_DIR "${_NM2_ROOT}/lib/linux/aarch64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
        # ARMv7 hard-float (Raspberry Pi 2/3/4 running 32-bit OS)
        set(_NM2_LIB_DIR "${_NM2_ROOT}/lib/linux/armhf")
    else()
        set(_NM2_LIB_DIR "${_NM2_ROOT}/lib/linux/x86_64")
    endif()
elseif(DEFINED PICO_BOARD)
    if(DEFINED FREERTOS_KERNEL_PATH OR DEFINED ENV{FREERTOS_KERNEL_PATH})
        set(_NM2_LIB_DIR "${_NM2_ROOT}/lib/pico/rp2350-rtos")
    elseif(PICO_BOARD MATCHES "pico2")
        set(_NM2_LIB_DIR "${_NM2_ROOT}/lib/pico/rp2350")
    else()
        set(_NM2_LIB_DIR "${_NM2_ROOT}/lib/pico/rp2040")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "cortex-m33"
       OR CMAKE_SYSTEM_PROCESSOR MATCHES "MCXN"
       OR (DEFINED MCUX_SDK_PATH AND CMAKE_CROSSCOMPILING))
    # NXP MCUXpresso SDK target (FRDM-MCXN947 or compatible Cortex-M33 board)
    set(_NM2_LIB_DIR "${_NM2_ROOT}/lib/nxp/mcxn947")
else()
    message(WARNING "NetworkMidi2: unrecognised platform — "
            "set NM2_LIB_DIR manually if needed.")
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
if(NOT TARGET NetworkMidi2::networkmidi2)
    add_library(NetworkMidi2::networkmidi2 STATIC IMPORTED GLOBAL)
    set_target_properties(NetworkMidi2::networkmidi2 PROPERTIES
        IMPORTED_LOCATION "${_NM2_LIB_DIR}/libnetworkmidi2.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_NM2_ROOT}/include"
    )
endif()

# ---------------------------------------------------------------------------
# POSIX transport (macOS / Linux desktop)
# ---------------------------------------------------------------------------
if(EXISTS "${_NM2_LIB_DIR}/libnm2_transport_posix.a")
    if(NOT TARGET NetworkMidi2::nm2_transport_posix)
        add_library(NetworkMidi2::nm2_transport_posix STATIC IMPORTED GLOBAL)
        set_target_properties(NetworkMidi2::nm2_transport_posix PROPERTIES
            IMPORTED_LOCATION "${_NM2_LIB_DIR}/libnm2_transport_posix.a"
            INTERFACE_INCLUDE_DIRECTORIES
                "${_NM2_ROOT}/transports/posix;${_NM2_ROOT}/include"
            INTERFACE_LINK_LIBRARIES "NetworkMidi2::networkmidi2"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# lwIP transport (Pico / embedded)
# ---------------------------------------------------------------------------
if(EXISTS "${_NM2_LIB_DIR}/libnm2_transport_lwip.a")
    if(NOT TARGET NetworkMidi2::nm2_transport_lwip)
        add_library(NetworkMidi2::nm2_transport_lwip STATIC IMPORTED GLOBAL)
        set_target_properties(NetworkMidi2::nm2_transport_lwip PROPERTIES
            IMPORTED_LOCATION "${_NM2_LIB_DIR}/libnm2_transport_lwip.a"
            INTERFACE_INCLUDE_DIRECTORIES
                "${_NM2_ROOT}/transports/lwip;${_NM2_ROOT}/include"
            INTERFACE_LINK_LIBRARIES "NetworkMidi2::networkmidi2"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# NXP MCUXpresso SDK transport (FRDM-MCXN947 / Cortex-M33 + lwIP + FreeRTOS)
# ---------------------------------------------------------------------------
if(EXISTS "${_NM2_LIB_DIR}/libnm2_transport_nxp.a")
    if(NOT TARGET NetworkMidi2::nm2_transport_nxp)
        add_library(NetworkMidi2::nm2_transport_nxp STATIC IMPORTED GLOBAL)
        set_target_properties(NetworkMidi2::nm2_transport_nxp PROPERTIES
            IMPORTED_LOCATION "${_NM2_LIB_DIR}/libnm2_transport_nxp.a"
            INTERFACE_INCLUDE_DIRECTORIES
                "${_NM2_ROOT}/transports/nxp;${_NM2_ROOT}/include"
            INTERFACE_LINK_LIBRARIES "NetworkMidi2::networkmidi2"
            INTERFACE_COMPILE_DEFINITIONS "NM2_HAVE_LWIP_MDNS=1"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# FreeRTOS+TCP transport
# ---------------------------------------------------------------------------
if(EXISTS "${_NM2_LIB_DIR}/libnm2_transport_freertos_plus_tcp.a")
    if(NOT TARGET NetworkMidi2::nm2_transport_freertos_plus_tcp)
        add_library(NetworkMidi2::nm2_transport_freertos_plus_tcp
                    STATIC IMPORTED GLOBAL)
        set_target_properties(NetworkMidi2::nm2_transport_freertos_plus_tcp
            PROPERTIES
            IMPORTED_LOCATION
                "${_NM2_LIB_DIR}/libnm2_transport_freertos_plus_tcp.a"
            INTERFACE_INCLUDE_DIRECTORIES
                "${_NM2_ROOT}/transports/freertos_plus_tcp;${_NM2_ROOT}/include"
            INTERFACE_LINK_LIBRARIES "NetworkMidi2::networkmidi2"
        )
    endif()
endif()

set(NetworkMidi2_FOUND TRUE)
