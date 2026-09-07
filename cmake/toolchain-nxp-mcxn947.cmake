# cmake/toolchain-nxp-mcxn947.cmake
#
# CMake toolchain file for NXP FRDM-MCXN947 (MCX N947, dual Cortex-M33 @ 150 MHz).
#
# Prerequisites:
#   arm-none-eabi-gcc toolchain on PATH, or ARMGCC_DIR set to the toolchain root.
#
# Usage:
#   cmake -B build_nxp \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-nxp-mcxn947.cmake \
#         -DMCUX_SDK_PATH=/path/to/sdk \
#         examples/midi_bridge/nxp
#   cmake --build build_nxp -j6
#
# The generated .elf can be converted to .bin / .hex via:
#   arm-none-eabi-objcopy -O binary nm2_nxp_mcxn947.elf nm2_nxp_mcxn947.bin
#   arm-none-eabi-objcopy -O ihex  nm2_nxp_mcxn947.elf nm2_nxp_mcxn947.hex
# Flash using MCUXpresso IDE, J-Link Commander, or pyOCD.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m33)

# ---------------------------------------------------------------------------
# Locate the arm-none-eabi toolchain
# ---------------------------------------------------------------------------
if(DEFINED ENV{ARMGCC_DIR} AND NOT ARMGCC_DIR)
    set(ARMGCC_DIR "$ENV{ARMGCC_DIR}")
endif()

if(ARMGCC_DIR)
    set(_NXP_TC_PREFIX "${ARMGCC_DIR}/bin/arm-none-eabi-")
else()
    # Expect the toolchain on PATH
    set(_NXP_TC_PREFIX "arm-none-eabi-")
endif()

set(CMAKE_C_COMPILER   "${_NXP_TC_PREFIX}gcc"     CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER "${_NXP_TC_PREFIX}g++"     CACHE FILEPATH "C++ compiler")
set(CMAKE_ASM_COMPILER "${_NXP_TC_PREFIX}gcc"     CACHE FILEPATH "ASM compiler")
set(CMAKE_AR           "${_NXP_TC_PREFIX}ar"      CACHE FILEPATH "Archiver")
set(CMAKE_OBJCOPY      "${_NXP_TC_PREFIX}objcopy" CACHE FILEPATH "objcopy")
set(CMAKE_OBJDUMP      "${_NXP_TC_PREFIX}objdump" CACHE FILEPATH "objdump")
set(CMAKE_SIZE         "${_NXP_TC_PREFIX}size"    CACHE FILEPATH "size")

# Prevent CMake from running compiler feature tests against the host system.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ---------------------------------------------------------------------------
# MCX N947 CPU flags
# Cortex-M33 with FPU (FPv5-SP-D16), hard-float ABI, Thumb ISA
# ---------------------------------------------------------------------------
set(_NXP_CPU_FLAGS
    "-mcpu=cortex-m33"
    "-mthumb"
    "-mfpu=fpv5-sp-d16"
    "-mfloat-abi=hard"
)
string(JOIN " " _NXP_CPU_FLAGS_STR ${_NXP_CPU_FLAGS})

set(CMAKE_C_FLAGS_INIT
    "${_NXP_CPU_FLAGS_STR} \
     -ffunction-sections \
     -fdata-sections \
     -fno-common \
     -Wall \
     -Wextra"
)

# No -ffreestanding: it disables libstdc++'s hosted-only headers (<map>,
# <functional>, ...), which third_party/AM_MIDI2.0Lib's midiCIProcessor.cpp
# needs -- the same library the Pico DEVICE-role build already links
# successfully, since its toolchain never set this flag. newlib-nano +
# nosys.specs (linker flags below) already constrain the C library to a
# bare-metal-appropriate subset; -ffreestanding was stricter than that
# actually required and blocked headers this project needs.
set(CMAKE_CXX_FLAGS_INIT
    "${_NXP_CPU_FLAGS_STR} \
     -ffunction-sections \
     -fdata-sections \
     -fno-common \
     -fno-exceptions \
     -fno-rtti \
     -Wall \
     -Wextra"
)

set(CMAKE_ASM_FLAGS_INIT
    "${_NXP_CPU_FLAGS_STR} -x assembler-with-cpp"
)

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${_NXP_CPU_FLAGS_STR} \
     -Wl,--gc-sections \
     -Wl,--print-memory-usage \
     -fno-common \
     -specs=nano.specs \
     -specs=nosys.specs"
)

# ---------------------------------------------------------------------------
# Build-type flags (optimisation + debug info)
# ---------------------------------------------------------------------------
set(CMAKE_C_FLAGS_DEBUG          "-Og -g3" CACHE STRING "")
set(CMAKE_CXX_FLAGS_DEBUG        "-Og -g3" CACHE STRING "")
set(CMAKE_C_FLAGS_RELEASE        "-O2 -DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE      "-O2 -DNDEBUG" CACHE STRING "")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG" CACHE STRING "")

# ---------------------------------------------------------------------------
# Search paths: only target sysroot, not host libraries
# ---------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
