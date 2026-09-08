# NetworkMIDI2 <-> USB MIDI 2.0 Bridge — Pico 2 W (WiFi, RP2350)

Bridges USB MIDI 2.0 (UMP) to Network MIDI 2.0 over WiFi, on a **Pico 2 W**
(RP2350 + cyw43 WiFi). This is the WiFi sibling of
[`examples/midi_bridge/pico`](../pico/README.md) (which targets plain
Pico/RP2040 + a WIZnet W5500 Ethernet expansion): it reuses that example's
composite USB descriptors, TinyUSB config, and (for HOST role) the patched
`hcd_rp2040.c` directly by path — those files are board-agnostic — but has
its **own** copies of `bridge_config.{h,cpp}`/`console_menu.{h,cpp}` (WiFi
credentials in place of static-IP fields) so nothing here can regress the
Ethernet target.

Two USB roles, selected at build time via `NM2_BRIDGE_USB_ROLE`:

| `NM2_BRIDGE_USB_ROLE` | Behavior |
|---|---|
| `DEVICE` (default) | The board presents as a class-compliant, composite USB device to a host computer/DAW: one interface is USB MIDI 2.0 (answering Endpoint/Function Block Discovery locally), the other a USB CDC serial console — and relays MIDI traffic to/from a NetworkMIDI2 session over WiFi. |
| `HOST` | The bridge is itself the USB host: plug a USB MIDI 2.0/1.0 device (or a hub with one attached) into the board's USB port, and its UMP traffic is bridged to/from a NetworkMIDI2 session over WiFi. Raw UMP pass-through only, same caveat as `examples/midi_bridge/pico`'s HOST role — the `tusb_ump` Host driver doesn't yet implement the UMP Stream-message handshake. USB port is occupied being the host port, so no CDC console (UART only). |

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
  in place — USB (including the CDC console, DEVICE role) stays enumerated
  throughout, no power cycle needed. Changing the WiFi SSID/password
  triggers an automatic WiFi reconnect; changing the Network MIDI name takes
  effect on the next power cycle.

Note: this "Role" is the **network session** role (Client connects out to a
NetworkMIDI2 peer, Host listens for one) — a separate axis from
`NM2_BRIDGE_USB_ROLE` above, which is a build-time choice about what the
board's own USB port does.

## Console

**DEVICE role:** USB CDC (part of the same composite USB device as the MIDI
interface) and UART both live at the same time — use whichever is
convenient. The CDC port lets a board with no Picoprobe/debug UART wired up
still have a console for the setup menu (`/dev/tty.usbmodem*` on macOS, at
115200 baud). Both are wired up via the Pico SDK's native dual-stdio
(`pico_enable_stdio_usb` + `pico_enable_stdio_uart`), no custom console
retarget code needed.

**HOST role:** UART only, same as `examples/midi_bridge/pico`'s HOST role —
the USB port is occupied being the host port, so it never had a CDC console
option.

## USB service architecture (both roles)

`tud_task()`/`tuh_task()` are pumped manually, from one tight loop
(`usb_task_tick()` in `main.cpp`), the same pattern every other target in
this project uses — **not** `pico_stdio_usb`'s IRQ-driven background task,
which this target used until it was found to be the cause of a DEVICE-role
enumeration failure (see Known issues below: this was the actual root
cause, not RP2350 silicon or TinyUSB). WiFi connect uses the non-blocking
`cyw43_arch_wifi_connect_async()` API with a locally-pumped poll loop
instead of the SDK's blocking `cyw43_arch_wifi_connect_timeout_ms()`
helper, specifically so `usb_task_tick()` never stops running — including
throughout WiFi connect/retry at boot, which is exactly when USB
enumeration also happens.

## Build

```sh
# DEVICE role (default):
cmake -B build_pico2w \
      -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
      -DPICO_BOARD=pico2_w \
      examples/midi_bridge/pico_w
cmake --build build_pico2w -j6

# HOST role:
cmake -B build_pico2w_host \
      -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0 \
      -DPICO_BOARD=pico2_w \
      -DNM2_BRIDGE_USB_ROLE=HOST \
      examples/midi_bridge/pico_w
cmake --build build_pico2w_host -j6
```

Flash the resulting `nm2_bridge_pico_w.uf2` to the Pico 2 W in BOOTSEL mode.

## Options

| CMake variable        | Values          | Default    | Notes |
|------------------------|-----------------|------------|-------|
| `PICO_BOARD`           | string          | `pico2_w`  | Target board — RP2350 + cyw43. |
| `NM2_BRIDGE_USB_ROLE`  | `HOST`/`DEVICE` | `DEVICE`   | See table above. |
| `NM2_MDNS_NAME`        | string          | `pico2w-nm2-bridge` | Base mDNS name; Host advertises `<name>-host.local`, Client looks for it. |

WiFi credentials and the NetworkMIDI2 session role are **runtime**
configuration now (see above) — there are no `NM2_WIFI_SSID`/
`NM2_WIFI_PASSWORD`/`NM2_BRIDGE_ROLE` CMake cache variables.

## Full-Speed USB, composite CDC+MIDI (DEVICE role)

Pico 2 W's RP2350 USB controller is Full-Speed, the same class of USB
controller as the plain Pico/RP2040 used by `examples/midi_bridge/pico` —
composite CDC+MIDI with 64-byte bulk endpoints (`usb_descriptors.cpp`) is
already proven working there. This is *not* the same situation as
`examples/midi_bridge/nxp` (FRDM-MCXN947), where a composite-CDC crash was
traced to that board's USB1 **High-Speed** ChipIdea controller, which
requires exactly 512-byte bulk endpoints — an MCU/TinyUSB-port-specific
issue unrelated to RP2040/RP2350's Full-Speed controllers.

## Known issues

- **Client role has no manual-IP-entry fallback on an unattended boot.** If
  mDNS discovery doesn't find a host and nobody is at the console, the
  board falls back to running as a USB-MIDI-device-only (no network
  session) rather than blocking on a prompt nobody can answer. With a
  console attached (CDC or UART), an interactive setup run always offers
  manual IP entry.
- **HOST role: an intermittent TinyUSB host panic** (`ep 80 was already
  available`) occurs on some boots, not correlated with any specific USB
  device plug/unplug state (reproduces even with nothing attached). Self-
  recovers via watchdog reset into a working state on the next boot — not a
  hard hang, but not yet root-caused. Likely an RP2350 host-controller
  timing issue; worth a dedicated debugging pass before relying on this
  role unattended.

### Resolved: DEVICE-role composite USB never enumerated on macOS

Earlier revisions of this target never enumerated its composite MIDI+CDC
device on macOS, and this was suspected to be an unresolved upstream RP2350
+ TinyUSB link-layout-timing bug (the class tracked at
`raspberrypi/pico-sdk#2216`). That suspicion was wrong. A USB protocol
analyzer capture (Beagle) showed the real sequence: `SET_ADDRESS` completes
normally, but the device never answers the host's very next
`GET_DESCRIPTOR` at the new address, forcing a bus reset and restarting
enumeration from scratch every time.

The actual cause: this was the only target in the project relying on
`pico_stdio_usb`'s IRQ-driven background task to call `tud_task()`, instead
of pumping it manually in a tight loop like every other target here — an
architecture change that happened to land alongside this RP2350 board for
no reason connected to WiFi, and was never exercised anywhere else in this
codebase. That background task raced against
`cyw43_arch_wifi_connect_timeout_ms()`'s blocking WiFi connect call (up to
45 s across its 3 retries) at boot, delaying the device-address hardware
write (`dcd_edpt0_status_complete()` in TinyUSB's `dcd_rp2040.c`) past the
host's next SETUP token. Fixed by reverting to a manually-pumped
`tud_task()`/`tuh_task()` loop for both USB roles and switching WiFi
connect to the non-blocking `cyw43_arch_wifi_connect_async()` API (see
"USB service architecture" above) — verified via `ioreg` on macOS: the
device now enumerates as `USBMidiNetworkBridge` and is claimed by
CoreMIDI's `MIDIServer`.

The RP2040-E15 bulk-IN uframe workaround is also now disabled for RP2350
DEVICE builds (`TUD_OPT_RP2040_USB_DEVICE_UFRAME_FIX=0`) — RP2350 fixes E15
in hardware and upstream TinyUSB already gates it to `rp2040` only; SDK
2.3.0's bundled 0.18.0 does not yet. This was tested and ruled out as the
actual cause (enumeration still failed with it disabled) but is correct to
leave disabled regardless.
