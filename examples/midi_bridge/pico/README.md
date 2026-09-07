# NetworkMIDI2 — Pico USB Bridge (RP2040)

Bridges USB MIDI 2.0 to **NetworkMIDI2** over a Wiznet W5500 Ethernet
expansion board, on a plain Pico (RP2040). Ported from AmeNote ProtoZOA's
`UUT/NetworkMIDI2_Bridge`, rebuilt to compile NetworkMIDI2's core from
source (this repo *is* NetworkMIDI2) instead of linking a prebuilt binary
distribution.

Two USB roles, selected at build time via `NM2_BRIDGE_USB_ROLE`:

| `NM2_BRIDGE_USB_ROLE` | Behavior |
|---|---|
| `HOST` (default) | The bridge is itself the USB host: plug a USB MIDI 2.0/1.0 device (or a hub with one attached) into the Pico's USB port, and its UMP traffic is bridged to/from a NetworkMIDI2 session over Ethernet. Raw UMP pass-through only — the `tusb_ump` Host driver does not yet implement the UMP Stream-message handshake (Endpoint/Function Block Discovery), so this role cannot answer or issue discovery over USB. |
| `DEVICE` | The bridge presents as a USB MIDI 2.0 device to a host computer/DAW, answering Endpoint/Function Block Discovery locally (`usb_descriptors.cpp`, `device/tusb_config.h`). Plug the Pico's USB port into a computer; MIDI flows between it and the NetworkMIDI2 session over Ethernet. |

Two W5500 wirings, selected via `NM2_WIZNET_BOARD` (applies to both USB
roles):

| `NM2_WIZNET_BOARD` | Board |
|---|---|
| `PROTOZOA` (default) | AmeNote ProtoZOA UUT's J13 expansion header (spi1, GPIO8-11/3) |
| `W5500_EVB_PICO` | WIZnet's own W5500-EVB-Pico eval board (spi0, GPIO16-20) |

## Known limitations / follow-up (not done in this port)

- **Other boards** (Pico W, Pico 2 / Pico 2 W, NXP FRDM-MCXN947, Raspberry Pi
  5 USB gadget mode) — future work, tracked separately.
- Hub-topology and device-timing quirks found during ProtoZOA bring-up
  (some devices failing to enumerate behind certain hubs, boot-time
  already-attached-device races) apply to the `HOST` role here too — see
  `tinyusb_overrides/hcd_rp2040.c`'s header comment for the one hardware bug
  it patches.

## Why `hcd_rp2040.c` is vendored

`tinyusb_overrides/hcd_rp2040.c` is a patched copy of pico-sdk's own RP2040
host-controller driver. On real hardware, a USB MIDI device that stops
responding mid-control-transfer produces a genuine `RX_TIMEOUT` interrupt
that the stock driver silently discards — no completion is ever reported to
TinyUSB core, so the single shared control endpoint (used by every attached
device) hangs forever with no diagnostic output. This patch reports it as
`XFER_RESULT_TIMEOUT` instead, letting TinyUSB's existing retry/give-up logic
run. See the file's own header comment for the full history/rationale.

## Hardware wiring

### W5500 SPI (default: `NM2_WIZNET_BOARD=PROTOZOA`)

| Signal      | RP2040 GPIO | Notes                          |
|-------------|-------------|---------------------------------|
| SPI_CS_N    | GPIO9       | spi1                            |
| SPI_SCK     | GPIO10      | spi1                            |
| SPI_MOSI    | GPIO11      | spi1                            |
| SPI_MISO    | GPIO8       | spi1 (hardware-confirmed pin)   |
| W5500_RST_N | GPIO3       |                                  |

Pass `-DNM2_WIZNET_BOARD=W5500_EVB_PICO` to instead target WIZnet's own
W5500-EVB-Pico eval board (spi0, GPIO16-20) — see `wiznet_port/w5x00_spi.h`.

### USB host power (VBUS)

A Pico's USB port normally *senses* VBUS supplied by whatever it's plugged
into. Running it in **USB HOST role** flips that: the board must instead
*supply* 5V onto its own VBUS pin so a downstream USB device can power up
and enumerate. Wire the Pico's own 5V rail onto its USB connector's VBUS pin
(directly, or via whatever power-switch/jumper your carrier board provides)
before attaching a device — without it, nothing will enumerate.

### Console

**Both roles: UART0 only.** Connect a USB-serial adapter (or a debug probe's
UART bridge) to the Pico's UART0 TX/RX pins at 115200 baud for the boot-time
setup menu and runtime logging — `pico_enable_stdio_uart(nm2_bridge_pico 1)` /
`pico_enable_stdio_usb(nm2_bridge_pico 0)`.

**HOST role:** the Pico's single USB peripheral is occupied by the bridge's
own HOST-role device enumeration, so it never had a CDC console option.

**DEVICE role:** used to also offer a USB CDC-ACM console (itf 2-3 in
`usb_descriptors.cpp`) alongside the MIDI function, active at the same time
as UART0. Pulled for this release: its RX direction could silently and
permanently wedge (keystrokes stop reaching the setup menu) after the host
reconnected to the CDC tty (e.g. macOS's `tio` dropping/reopening the port)
— traced to `pico_stdio_usb`'s background IRQ task racing the app's own
`tud_task()` calls, both non-reentrant against each other. See git log ("fix
CDC console RX wedging") for the fix and the descriptor bytes if reintroduced
later with more field validation. `main.cpp` still brings `tusb_init()` up
*before* `stdio_init_all()` in this role and pulses
`tud_disconnect()`/`tud_connect()` at boot so a debugger-issued reset (e.g.
reflashing over a picoprobe without a physical cable replug) reliably forces
the host to re-read the new descriptor set instead of keeping a stale cached
one — that part is unrelated to the CDC console and still applies to the
MIDI-only descriptor.

## Build

```sh
# HOST role (default), ProtoZOA wiring (default):
cmake -B build_pico \
      -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
      -DPICO_BOARD=pico \
      examples/midi_bridge/pico
cmake --build build_pico -j6

# DEVICE role, ProtoZOA wiring:
cmake -B build_pico_device \
      -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
      -DPICO_BOARD=pico \
      -DNM2_BRIDGE_USB_ROLE=DEVICE \
      examples/midi_bridge/pico
cmake --build build_pico_device -j6

# DEVICE role, WIZnet W5500-EVB-Pico eval board:
cmake -B build_pico_device_evb \
      -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
      -DPICO_BOARD=pico \
      -DNM2_BRIDGE_USB_ROLE=DEVICE \
      -DNM2_WIZNET_BOARD=W5500_EVB_PICO \
      examples/midi_bridge/pico
cmake --build build_pico_device_evb -j6

# HOST role, WIZnet W5500-EVB-Pico eval board:
cmake -B build_pico_host_evb \
      -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
      -DPICO_BOARD=pico \
      -DNM2_BRIDGE_USB_ROLE=HOST \
      -DNM2_WIZNET_BOARD=W5500_EVB_PICO \
      examples/midi_bridge/pico
cmake --build build_pico_host_evb -j6
```

Flash the resulting `nm2_bridge_pico.uf2` to a Pico held in BOOTSEL mode.

## Runtime configuration

Same boot-time serial setup menu as ProtoZOA's bridge (role Host/Client,
device name, DHCP vs. static IP, mDNS host discovery for Client role),
persisted to the last flash sector. Press ESC at any time while running to
reboot into setup.

## Dependencies

- `third_party/tusb_ump` (submodule, `AmeNote-Michael/tusb_ump`,
  `feature/usb-host-ump` branch) — USB HOST UMP class driver. Private fork;
  cloning requires access to the `AmeNote-Michael` GitHub account.
- `third_party/ioLibrary_Driver` (submodule, `Wiznet/ioLibrary_Driver`,
  public) — W5500 chip driver (`socket.c`, `wizchip_conf.c`, `W5500/w5500.c`).
  Deliberately added as the standalone WIZnet driver repo rather than the
  `RP2040-HAT-LWIP-C` wrapper, which drags in a large nested `pico-sdk`
  submodule for no benefit here.
- `third_party/AM_MIDI2.0Lib` (submodule, `midi2-dev/AM_MIDI2.0Lib`, public)
  — UMP message helpers (`UMPMessage`, `umpProcessor`) used by the DEVICE
  role's local Endpoint/Function Block Discovery replies. Linked into both
  roles' builds (unused dead code in the HOST role), matching ProtoZOA. Its
  own `libmidi2_test` executable is excluded from the build
  (`EXCLUDE_FROM_ALL`) — it's a native x86 test harness that doesn't
  cross-compile under the ARM/newlib toolchain.
