# NetworkMIDI2 <-> USB MIDI 2.0 Bridge — Pico 2 W (WiFi, USB DEVICE role)

Bridges a real USB MIDI 2.0 (UMP) device interface on a **Pico 2 W**
(RP2350 + cyw43 WiFi) to Network MIDI 2.0 over WiFi. The board presents
itself as a class-compliant, composite USB device to a host computer/DAW:
one interface is USB MIDI 2.0 (answering Endpoint/Function Block Discovery
locally), the other is a USB CDC serial console — and relays MIDI traffic
to/from a NetworkMIDI2 session over the board's WiFi radio.

This is the WiFi sibling of [`examples/midi_bridge/pico`](../pico/README.md)
(which targets plain Pico/RP2040 + a WIZnet W5500 Ethernet expansion). It
reuses that example's composite USB descriptors and TinyUSB config
(`../pico/usb_descriptors.cpp`, `../pico/device/tusb_config.h`) directly by
path — those files are board-agnostic — but has its **own** copies of
`bridge_config.{h,cpp}`/`console_menu.{h,cpp}` (WiFi credentials in place of
static-IP fields) so nothing here can regress the Ethernet target.

There is no USB **HOST** role for this target — ProtoZOA's HOST role exists
specifically to bridge a directly-attached USB MIDI keyboard, which is a
separate concern from WiFi transport and isn't needed here.

## Runtime configuration (no rebuild needed to change WiFi/role/name)

Role, device name, and WiFi SSID/password are **not** compile-time
settings. They're stored in flash and set via a boot-time console menu, the
same pattern as `examples/midi_bridge/pico`:

- On boot, the board prints its current configuration and waits ~3 seconds
  for a keypress to enter setup. If nothing is pressed, it continues with
  the saved (or default) configuration.
- If no WiFi SSID has ever been configured, setup runs automatically
  (there's no "run without WiFi" fallback — this build's whole purpose is
  the network bridge).
- Setup walks: **Role** (Client/Host) -> **Network MIDI name** -> **WiFi
  SSID** (`WiFi SSID (Enter to use saved, or type new): `) -> **WiFi
  password** (`WiFi password (Enter to use saved): `) -> (Client role only)
  a host picked from mDNS discovery or entered manually.
- Press **ESC** at any time while the bridge is running to re-enter setup
  in place — USB (including the CDC console) stays enumerated throughout,
  no power cycle needed. Changing the WiFi SSID/password triggers an
  automatic WiFi reconnect; changing the Network MIDI name takes effect on
  the next power cycle.

## Console: USB CDC or UART, your choice

The composite USB device exposes a CDC serial port alongside the MIDI
interface, so a board with no Picoprobe/debug UART wired up still has a
console for the setup menu — just open the CDC port that shows up when you
plug in (`/dev/tty.usbmodem*` on macOS) at 115200 baud. The physical UART
console (via a Picoprobe or other debug probe) also stays live at the same
time; use whichever is convenient. Both are wired up via the Pico SDK's
native dual-stdio (`pico_enable_stdio_usb` + `pico_enable_stdio_uart`), no
custom console retarget code needed.

## Build

```sh
cmake -B build_pico2w \
      -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
      -DPICO_BOARD=pico2_w \
      examples/midi_bridge/pico_w
cmake --build build_pico2w -j6
```

Flash `build_pico2w/nm2_bridge_pico_w.uf2` to the Pico 2 W held in BOOTSEL
mode.

## Options

| CMake variable    | Values | Default              | Notes |
|--------------------|--------|-----------------------|-------|
| `PICO_BOARD`       | string | `pico2_w`             | Target board — RP2350 + cyw43. |
| `NM2_MDNS_NAME`    | string | `pico2w-nm2-bridge`   | Base mDNS name; Host advertises `<name>-host.local`, Client looks for it. |

WiFi credentials and the NetworkMIDI2 session role are **runtime**
configuration now (see above) — there are no `NM2_WIFI_SSID`/
`NM2_WIFI_PASSWORD`/`NM2_BRIDGE_ROLE` CMake cache variables.

## Full-Speed USB, composite CDC+MIDI

Pico 2 W's RP2350 USB controller is Full-Speed, the same class of USB
controller as the plain Pico/RP2040 used by `examples/midi_bridge/pico` —
composite CDC+MIDI with 64-byte bulk endpoints (`usb_descriptors.cpp`) is
already proven working there. This is *not* the same situation as
`examples/midi_bridge/nxp` (FRDM-MCXN947), where a composite-CDC crash was
traced to that board's USB1 **High-Speed** ChipIdea controller, which
requires exactly 512-byte bulk endpoints — an MCU/TinyUSB-port-specific
issue unrelated to RP2040/RP2350's Full-Speed controllers.

## Known limitations

- **Client role has no manual-IP-entry fallback on an unattended boot.** If
  mDNS discovery doesn't find a host and nobody is at the console, the
  board falls back to running as a USB-MIDI-device-only (no network
  session) rather than blocking on a prompt nobody can answer. With a
  console attached (CDC or UART), an interactive setup run always offers
  manual IP entry.
- **USB (device-role composite MIDI+CDC) does not currently enumerate
  reliably on this board, paused pending an upstream fix.** Extensively
  bench-tested on a real Pico 2 W + Picoprobe: the app itself boots and
  runs correctly (UART console, config menu, WiFi connect all confirmed
  working), but the USB device never finishes enumerating on macOS.
  Isolated with high confidence to a link-layout-sensitive RP2350 +
  TinyUSB device-controller timing bug -- reproduces with 100% stock
  TinyUSB descriptors/class driver (not this project's own code), and
  `dcd_rp2040.c` compiles to byte-identical object code across a failing
  and a working link configuration, with only the final link layout
  differing. Matches the class of bug tracked upstream at
  `raspberrypi/pico-sdk#2216` (open as of pico-sdk 2.3.1, targeted for
  2.4.0). Tried and ruled out: ARM GCC 13.3.1 vs. ARM LLVM/clang 18.1.3
  (both fail identically), the already-fixed RP2350 DPRAM
  `unaligned_memcpy` hardfault (tinyusb PR #3585, backported locally --
  still fails), and an incidental link-layout workaround that "fixed" a
  minimal repro but did not generalize to this full application. A
  RISC-V (Hazard3) toolchain build was attempted but left inconclusive:
  `pyocd` has no RISC-V debug support for RP2350 at all, so there was no
  way to verify it with the tooling on hand. Revisit once pico-sdk 2.4.0
  ships, or with a UART-only (no `pyocd`) verification method for the
  RISC-V path.
