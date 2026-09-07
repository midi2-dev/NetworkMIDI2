# NetworkMIDI2 — FRDM-MCXN947 (FreeRTOS + lwIP + USB MIDI 2.0 device/host)

USB MIDI 2.0 (UMP) bridge for the NXP FRDM-MCXN947, bridged to NetworkMIDI2
over the board's onboard Ethernet (ENET, LAN8741 PHY). Two USB roles,
selected at build time via `NM2_BRIDGE_USB_ROLE` (see CMakeLists.txt),
same toggle/pattern as `examples/midi_bridge/pico/`:

- **DEVICE** (default) — a class-compliant USB MIDI 2.0 device interface
  (USB1, ChipIdea High-Speed controller) presented to a host computer/DAW,
  answering Endpoint/Function Block Discovery locally.
- **HOST** — the bridge is itself the USB host (same USB1 HS controller,
  now running TinyUSB's chipidea host/EHCI stack instead), bridging to a
  directly- or hub-attached USB MIDI 2.0/UMP device. That driver doesn't
  implement UMP Stream-message discovery yet (see
  `third_party/tusb_ump/ump_host.h`'s scope notes), so the HOST role is a
  raw UMP pass-through in both directions, no discovery handling — same
  limitation as `examples/midi_bridge/pico/`'s HOST role and
  `UUT/USB_Host_UMP_Test`. Requires a cable/adapter capable of sourcing
  VBUS to the attached device (confirmed working on this board's USB1
  connector with a standard USB-A-to-USB-C/micro host cable — no board
  modification needed). **Power note:** this board has no separate power
  input — it's powered entirely through the MCU-Link connector, which
  therefore has to supply both the board's own draw and the attached USB
  MIDI device's draw (routed through via USB1). A current-limited
  computer USB port (common on laptops, or an unpowered hub) can starve a
  higher-current downstream device; if a device fails to enumerate, drops
  out, or misbehaves only in HOST role, try a different host port or a
  powered hub ahead of the MCU-Link connection before suspecting the
  firmware.

## What it does

- Presents a class-compliant USB MIDI 2.0 (UMP) interface to the host
  computer (`third_party/tusb_ump`'s `ump_device.cpp`, same driver used by
  the Pico DEVICE-role builds).
- Bridges UMP traffic between that USB interface and a NetworkMIDI2 session
  over Ethernet/DHCP, with mDNS discovery (unchanged from the pre-existing
  ENET/lwIP/FreeRTOS scaffolding). The console UX (role/name setup, ESC to
  reconfigure) is unified with the Pico DEVICE-role build's wording and
  flow — see `SessionTask.cpp`'s file header comment for what's shared vs.
  intentionally different.
- Answers the USB host's UMP Endpoint/Function Block Discovery requests
  locally (`SessionTask.cpp`'s `midiendpoint()`/`functionblock()`, using
  `third_party/AM_MIDI2.0Lib`'s `umpProcessor`/`UMPMessage`) — same structure
  as the Pico DEVICE-role build (`examples/midi_bridge/pico/main.cpp`) and
  `UUT/DIN_Bridge`. Without this, CoreMIDI-class hosts (macOS, and similar
  native-UMP stacks) never finish claiming the device as a real MIDI
  endpoint — it stays enumerated at the USB level but invisible to MIDI
  applications, which is what this example originally shipped with.

## Operation

This section describes how the firmware behaves once flashed and running —
the boot sequence, the console setup menu, and what each USB role actually
does at runtime. It does not cover how a Windows or macOS computer should
be connected/configured to talk to this bridge — that's a follow-up.

The console is the MCU-Link virtual COM port (LPUART4, 115200 baud, 8N1) —
identical for both USB roles, since USB1 is entirely committed to the
MIDI interface (device or host) and never carries a CDC console itself.

### Boot sequence

1. **Current configuration + 3-second setup gate**, printed immediately
   (before Ethernet is even brought up — same order as the Pico builds:
   decide configuration first, bring the network up per that decision
   second):
   ```
   Current configuration:
     Role:    Client
     Name:    nxpmidi
     Network: DHCP / link-local
     (not persisted across reboots on this board)

   Press any key within 3 seconds to enter setup (or press ESC any time later while the bridge is running)...
   ```
   `Role` here is the NetworkMIDI2 session role (Client/Host — see below),
   unrelated to the build-time USB role (DEVICE/HOST). `(not persisted...)`
   is a real limitation: unlike the Pico builds, this configuration lives
   in RAM only and reverts to defaults (`Client`, name `nxpmidi`, DHCP) on
   every reset/power-cycle. Press any key to enter the setup menu
   immediately; otherwise the board continues with the printed
   configuration once the 3 s window elapses. If UART RX isn't wired up on
   the bench (no key can ever arrive), each setup prompt auto-advances
   after 60 s using its default/most recent value, so the board still
   reaches a running state unattended.
2. **Network comes up** per the decided DHCP-vs-static choice (see setup
   menu below): `IP: 10.0.0.212  (DHCP)` or `IP: 10.0.0.150  (static)`. For
   DHCP, a link/lease-progress heartbeat prints every 5 s while waiting (up
   to 30 s before retrying); a static address takes effect on the next
   tick, no waiting. (HOST role only: the USB host controller also starts
   enumerating any already-attached USB MIDI device independently of this
   — a `[...] USB HOST: UMP device mounted daddr=<n> itf_num=<n>` line can
   appear at any point, unrelated to network timing.)

### Setup menu (entered via the 3 s gate, or ESC at any time afterward)

1. `Role -- [C]lient or [H]ost [C]:` — the **NetworkMIDI2 session role**
   (which side sends the initial Invitation), not the USB role. Enter
   alone keeps the current value.
2. `Network MIDI name [nxpmidi]:` — the endpoint's advertised/mDNS name.
   Enter alone keeps the current value (or the `nxpmidi` default on first
   boot).
3. `Network -- [D]HCP/link-local or [S]tatic IP [D]:` — Enter alone keeps
   the current mode. Choosing **Static** prompts for four more fields in
   turn, each showing the current/default value in brackets (Enter alone
   keeps it):
   ```
     Static IP     [192.168.1.200]:
     Subnet mask   [255.255.255.0]:
     Gateway       [192.168.1.1]:
     DNS server    [192.168.1.1]:
   ```
   An invalid IPv4 address re-prompts the same field. This takes effect
   immediately, live — no reboot needed, even when switching from DHCP to
   static or back on an already-running board via ESC (confirmed:
   re-pinging the board's old DHCP address after switching to static shows
   it no longer responds, while the new static address does).
4. **If Host** was selected: once the network is up, the session starts
   listening immediately (`beginHost()`, UDP port 5004) — see "Bridge
   running" below.
5. **If Client** was selected: once the network is up, the board browses
   mDNS (`_midi2._udp`) for up to 4 seconds, printing each discovered peer
   as it's found (`  [1] <name>  <ip>:<port>`), then either:
   - prompts `Select host [1-N]:` if any were found (Enter with none
     selected falls through to manual entry), or
   - prompts `Host IP:` directly if none were found — enter a dotted-quad
     address (port is always 5004; an invalid address re-prompts).

Once a role/host is resolved, the board prints `Bridge running. Press ESC
at any time to re-enter setup.` and enters its steady-state run loop.

### ESC — reconfigure without a reset

Pressing ESC at any time during the run loop re-enters the setup menu
above **in place** — no reboot, no USB re-enumeration/detach (DEVICE role)
and no loss of the currently-mounted USB device (HOST role: the attached
device stays enumerated; only the NetworkMIDI2 session is torn down and
rebuilt). Matches the Pico builds' ESC behavior/rationale exactly.

### Session states

Printed as `[<uptime-ms>] NM2 session state -> <name>` on every
transition:

| State | Meaning |
|---|---|
| `Idle` | No active session (Host role: listening for an Invitation; freshly closed) |
| `PendingInvitation` | Client role: Invitation sent, awaiting reply |
| `AuthRequired` | Peer requires SHA-256 shared-secret auth (not used by this example's default build) |
| `Established` | Session live — MIDI data flows in both directions |
| `PendingReset` / `PendingBye` | Session tearing down |

While in `Host` role with no peer connected yet, a heartbeat prints every
4 s: `[Host] Still waiting at <ip>:5004  rx=<n>`.

### DEVICE role: what the computer sees

The board enumerates as a class-compliant USB MIDI 2.0 (UMP) device named
`USBMidiNetworkBridge`, answering Endpoint/Function Block Discovery
locally so CoreMIDI-class hosts claim it as a real MIDI endpoint (not just
a generic USB device). Every UMP word received from the USB side is
logged (`[UMP] ...`) and forwarded into the NetworkMIDI2 session once
Established; every UMP word received from the network is forwarded back
out the USB interface.

### HOST role: what happens with an attached USB MIDI device

The board itself acts as the USB host. Attach a USB MIDI 2.0/UMP device
(directly, or through a hub) to the USB1 connector using a cable/adapter
capable of sourcing VBUS to it — confirmed working with a standard host
cable, no board modification needed. On successful enumeration:
`[<uptime-ms>] USB HOST: UMP device mounted daddr=<n> itf_num=<n>`
(and the corresponding `unmounted` line on detach). Only one downstream
device is tracked at a time (see `s_usbHostMounted` in `SessionTask.cpp`).
Unlike the DEVICE role, there is **no local Endpoint/Function Block
Discovery** — `third_party/tusb_ump`'s host driver doesn't implement the
UMP Stream-message handshake yet, so every UMP word from the attached
device forwards straight to the network session (once Established), and
every word received from the network forwards straight out to the
attached device, with no discovery filtering in either direction.

## USB1 High-Speed controller

FRDM-MCXN947's USB-C connector is wired to **USB1** (rhport 1, ChipIdea
High-Speed controller); **USB0** (KHCI, Full-Speed) is unused here. TinyUSB
has first-class board support for this exact board
(`third_party/tinyusb/hw/bsp/mcx/boards/frdm_mcxn947`), confirmed present at
the same TinyUSB commit (0.18.0) that pico-sdk 2.3.0 bundles — the version
`third_party/tusb_ump` was already proven against by the Pico builds.

This example does **not** use TinyUSB's own `board.c`/`pin_mux.c`/
`clock_config.c` for that board — those are NXP Config-Tools-generated files
that would collide (same function names: `BOARD_InitBootPins()`,
`BOARD_InitBootClocks()`, ...) with this example's own Ethernet-focused board
init (`board_init.cpp`, from the pre-existing ENET/lwIP example). Instead,
`NM2_UsbHsInit()` in `board_init.cpp` carries just the USB1-HS-specific
power/clock/PHY register sequence, ported verbatim (register-level only, no
NXP-SDK-driver dependency beyond `fsl_clock`) from TinyUSB's own
`hw/bsp/mcx/family.c` `board_init()`. Only TinyUSB's core/device sources and
its `portable/chipidea/ci_hs/dcd_ci_hs.c` driver are built from the
`third_party/tinyusb` submodule.

USB bulk endpoints are declared 512 bytes in `usb_descriptors.cpp` -- this is
not optional. USB 2.0 High-Speed bulk endpoints must be *exactly* 512 bytes
(TinyUSB's `tu_edpt_validate()` enforces this and stalls `SET_CONFIGURATION`
otherwise); the 64-byte value copied from the Full-Speed-only Pico/ProtoZOA
builds is invalid here. An earlier version of this file claimed 64 bytes was
"full-speed-compatible... but isn't required for correct operation" -- that
was wrong and was the actual root cause of a real bug: the device enumerated
far enough for a host to read its VID/PID/strings, but `SET_CONFIGURATION`
silently stalled EP0 every time (visible with `-DCFG_TUSB_DEBUG=2`, see
below), so no host ever finished configuring it -- macOS's CoreMIDI, for one,
never claimed it as a MIDI destination despite the device otherwise looking
correct at the USB level.

### Verbose TinyUSB logging

Configure with `-DCFG_TUSB_DEBUG=<0-3>` (default 0) for device-stack activity
over the LPUART console -- unlike the RP2040 examples, this build doesn't go
through TinyUSB's `hw/bsp/family.cmake` helper (which force-injects
`CFG_TUSB_DEBUG=0` for Release builds on that platform), so the define takes
effect directly. This is how the bulk-endpoint-size bug above was found.

## Task structure

Three FreeRTOS tasks, created in `main.cpp` before `vTaskStartScheduler()`:

| Task          | Priority                    | Role                                             |
|---------------|------------------------------|---------------------------------------------------|
| `usbd`        | `configMAX_PRIORITIES - 1`  | `tusb_init()` + `tud_task()` loop (USB1 HS)        |
| `session`     | 2                            | Ethernet wait, mDNS, auth, NetworkMIDI2 session, CLI |

`tusb_init()` is deliberately called from *inside* the `usbd` task (after the
scheduler starts), not from `main()` — TinyUSB's FreeRTOS OSAL uses queue/
semaphore APIs internally once `CFG_TUSB_OS=OPT_OS_FREERTOS`, which requires
the scheduler already running.

`SessionTask.cpp`'s main loop (DEVICE role) polls `tud_ump_read_ntoh()` and
forwards to `NetworkMidiSession::sendUmp()`; the session's `onUmp` callback
forwards network-received UMP out via `tud_ump_write_hton()`. The HOST role
uses the same structure with `tuh_ump_read_ntoh()`/`tuh_ump_write_hton()`
against the single currently-mounted device (`s_usbHostDaddr`/
`s_usbHostItfNum`) instead — same pattern the Pico HOST-role build uses.

## Build

```sh
cmake -B build_nxp \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-nxp-mcxn947.cmake \
      -DMCUX_SDK_PATH=~/sdk/SDK_2_16_100_FRDM-MCXN947 \
      examples/midi_bridge/nxp
cmake --build build_nxp -j6
```

Add `-DNM2_BRIDGE_USB_ROLE=HOST` to build the HOST role instead (default is
`DEVICE`). The build produces `nm2_nxp_mcxn947.elf`, `.hex`, and `.bin` in
the build directory — any of the three can be flashed.

## Flashing (no debugger/IDE required)

Connect the FRDM-MCXN947 to the computer with a USB cable at the
**MCU-Link** connector (labeled on the board, separate from the USB1
connector used by this firmware's own MIDI interface). This exposes the
MCU-Link probe over USB — no separate debug probe hardware is needed, and
none of the steps below require MCUXpresso IDE, pyOCD, or J-Link
Commander to be installed.

1. Install **[LinkServer](https://www.nxp.com/design/design-center/software/development-software/mcuxpresso-software-and-tools/linkserver-debugger:LINKSERVER)**
   (NXP's free flashing/debug-probe utility for MCU-Link — a standalone
   download, or already present if MCUXpresso IDE is installed, e.g. at
   `/Applications/LinkServer_<version>/LinkServer` on macOS).
2. With the board connected, flash with:
   ```sh
   LinkServer flash MCXN947 load nm2_nxp_mcxn947.hex
   ```
   (substitute the actual path to the `.hex` file you were given, or
   `.elf`/`.bin` — LinkServer detects the format from the extension. A
   `.bin` file needs an explicit load address: add
   `-a 0x0` for this board's flash layout, or ask for a `.hex`/`.elf`
   instead to avoid needing one.) This erases only the sectors being
   written and resets the board to start running the new firmware when
   done — no separate "run"/"reset" step needed.
3. If more than one probe is connected to the computer, LinkServer will
   list them and ask you to pick one via `-p <serial-or-index>` (see
   `LinkServer flash --help`).

## Serial console

The MCU-Link connector also exposes a **virtual COM port** (LPUART4,
115200 baud, 8 data bits, no parity, 1 stop bit — "115200 8N1") — this is
the firmware's entire console (setup menu, session status, MIDI activity
log). Open it in any serial terminal program:

- **macOS**: find the port name with `ls /dev/cu.usbmodem*`, then
  `screen /dev/cu.usbmodemXXXXXXXX 115200` (exit with `Ctrl-A` then `k`,
  then `y` to confirm). A GUI alternative:
  [CoolTerm](https://freeware.the-meiers.org/) or the Serial extension in
  VS Code.
- **Windows**: open Device Manager → Ports (COM & LPT) to find the COM
  port number for "MCU-LINK ... CMSIS-DAP" (or similar), then open it in
  [Tera Term](https://teratermproject.github.io/index-en.html) or
  [PuTTY](https://www.putty.org/) (connection type "Serial", speed
  115200).
- **Linux**: `ls /dev/ttyACM*` (or `/dev/ttyUSB*`) to find the port, then
  `screen /dev/ttyACM0 115200` (or use `minicom`/`picocom`).

Reset the board (power-cycle, or the reset button if the board has one)
after opening the terminal to see the full boot sequence from the start —
see "Operation" above for what to expect.

## Known limitations / follow-ups

- **USB serial number string is a placeholder** (`"abcd1234"` in
  `usb_descriptors.cpp`'s string table, DEVICE role only) — the Pico builds
  derive a real per-board serial from `pico_get_unique_board_id_string()`;
  MCXN947 has no equivalent wired up yet (the SDK exposes a unique ID via
  `OCOTP`/UUID registers — worth deriving from that for multi-board
  deployments, same motivation as the W5500 MAC-randomization fix on the
  Pico/ProtoZOA side).
- **SRAM usage is high** (~81% of 384 KB DEVICE role, ~95% HOST role, in a
  Release build) — mostly ENET buffer descriptors, the FreeRTOS heap
  (`configTOTAL_HEAP_SIZE`, `FreeRTOSConfig.h`), and, for the HOST role,
  TinyUSB's static EHCI queue-head/qTD pools (`host/tusb_config.h`'s
  `CFG_TUH_DEVICE_MAX`/`CFG_TUH_ENDPOINT_MAX`/`CFG_TUH_HUB` are trimmed
  hard from the Pico HOST role's generous defaults specifically to fit —
  see that file's comment for the sizing rationale). Worth profiling
  further if more features are added to either role.
- **HOST role: `hcd_ci_hs.c` needs a locally patched copy**
  (`tinyusb_overrides/hcd_ci_hs.c`) to add an `OPT_MCU_MCXN9` branch that
  only exists upstream on its DEVICE-role sibling (`dcd_ci_hs.c`) — a
  one-line TinyUSB gap, not a real MCX-N9-host-mode limitation. Re-sync
  against the submodule's copy if TinyUSB is ever updated.
- **HOST role: Network→USB direction is not independently confirmed** with
  a device that gives visible feedback — USB→Network was verified with
  live, continuous traffic from a real attached device; the reverse
  direction uses the identical `tuh_ump_write_hton()` API (same driver,
  same endpoint machinery, just outbound) and reports no error, but hasn't
  been visually/audibly confirmed end-to-end yet.
- TinyUSB's own build (`add_tinyusb()` in `third_party/tinyusb/src/CMakeLists.txt`)
  compiles with `-Werror`; this example cancels it for the `nm2_nxp_tinyusb`
  target (`-Wno-error`) because the NXP SDK's `SystemCoreClock` is validly
  declared twice (once by `system_MCXN947_cm33_core0.h`, once by this
  example's own `FreeRTOSConfig.h`) and TinyUSB's strict CI policy treats that
  as fatal. The underlying warning is harmless; downstream integrations don't
  need TinyUSB's own CI strictness.
