# TinyUSB for the Pico-family bridges (examples/midi_bridge/pico, pico_w).
# Include BEFORE pico_sdk_import.cmake: the SDK reads PICO_TINYUSB_PATH there.
#
# Default: third_party/tinyusb-0.21 -- upstream TinyUSB 0.21.0 plus the USB 3.x
# EP0 max-packet fix (midi2-dev/tinyusb branch nm2/0.21.0; hathach/tinyusb#3930).
# The Pico SDK 2.3.0 ships TinyUSB 0.18 (Dec 2024), whose RP2040 host driver
# runs bulk endpoints on the 1 ms interrupt-endpoint pollers: a Pico bridge in
# HOST role sent one message per millisecond to a USB MIDI device. 0.21 runs
# bulk on EPX; measured on a ProtoZOA, network -> USB chords went from 1 ms to
# 0.16 ms between notes and ~2 ms to ~0.8 ms average latency.
#
# To build against the SDK's own TinyUSB instead (the previous behaviour):
#   -DPICO_TINYUSB_PATH=$PICO_SDK_PATH/lib/tinyusb
# NM2_PICO_HCD_OVERRIDE then defaults ON, restoring our patched hcd_rp2040.c,
# which is written for 0.18 and is not used with 0.21.

set(NM2_PICO_TINYUSB_021 "${CMAKE_CURRENT_LIST_DIR}/../third_party/tinyusb-0.21")
get_filename_component(NM2_PICO_TINYUSB_021 "${NM2_PICO_TINYUSB_021}" ABSOLUTE)

if(NOT PICO_TINYUSB_PATH AND NOT DEFINED ENV{PICO_TINYUSB_PATH})
    if(EXISTS "${NM2_PICO_TINYUSB_021}/src/tusb.h")
        set(PICO_TINYUSB_PATH "${NM2_PICO_TINYUSB_021}" CACHE PATH "TinyUSB used by the Pico SDK")
    else()
        message(WARNING "third_party/tinyusb-0.21 is not checked out "
                        "(git submodule update --init third_party/tinyusb-0.21); "
                        "falling back to the Pico SDK's TinyUSB")
    endif()
endif()

if(PICO_TINYUSB_PATH STREQUAL NM2_PICO_TINYUSB_021)
    set(_nm2_hcd_override_default OFF)
else()
    set(_nm2_hcd_override_default ON)
endif()
option(NM2_PICO_HCD_OVERRIDE
       "HOST role: use examples/midi_bridge/pico/tinyusb_overrides/hcd_rp2040.c (TinyUSB 0.18 only)"
       ${_nm2_hcd_override_default})
message(STATUS "Pico TinyUSB: ${PICO_TINYUSB_PATH} (hcd_rp2040 override: ${NM2_PICO_HCD_OVERRIDE})")
