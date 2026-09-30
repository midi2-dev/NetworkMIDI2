# System clock for the Pico-family bridges (examples/midi_bridge/pico, pico_w).
# Include before pico_sdk_init().
#
# NM2_PICO_FAST_CLOCK (default ON) runs the chip at the fastest clock the Pico
# SDK officially supports -- 200 MHz on the RP2040 instead of 125 MHz (the SDK
# sets the PLL and, where needed, the core voltage; USB keeps its own 48 MHz
# clock). Inbound Network MIDI is CPU-bound per datagram: a W5500-EVB-Pico
# took ~2,200 datagrams/s at 125 MHz, below what a Windows peer sends with
# 256 B+ SysEx (one command of ~3 UMP packets per datagram: 3,000-4,500/s).
# OFF restores the SDK default clock.
#
# NM2_PICO_SYS_CLK_MHZ pins a specific clock instead, overriding both: for
# like-for-like comparisons between boards, e.g. 150 to match the RP2350 in a
# Pico 2 W. The SDK derives the PLL for 125, 150 and (RP2040) 200 MHz; other
# values need PLL_SYS_* definitions too.
option(NM2_PICO_FAST_CLOCK "Run at the fastest officially supported clock (RP2040: 200 MHz)" ON)
set(NM2_PICO_SYS_CLK_MHZ "" CACHE STRING "Pin the system clock in MHz (empty = NM2_PICO_FAST_CLOCK decides)")
if(NM2_PICO_SYS_CLK_MHZ)
    math(EXPR _nm2_sys_clk_hz "${NM2_PICO_SYS_CLK_MHZ} * 1000000")
    add_compile_definitions(SYS_CLK_HZ=${_nm2_sys_clk_hz})
    message(STATUS "Pico clock: pinned to ${NM2_PICO_SYS_CLK_MHZ} MHz")
else()
    if(NM2_PICO_FAST_CLOCK)
        set(PICO_USE_FASTEST_SUPPORTED_CLOCK 1)
    endif()
    message(STATUS "Pico clock: ${NM2_PICO_FAST_CLOCK} (PICO_USE_FASTEST_SUPPORTED_CLOCK=${PICO_USE_FASTEST_SUPPORTED_CLOCK})")
endif()
