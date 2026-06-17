# NetworkMIDI2 Release Notes

## v0.2.0 — June 2026

Adds NXP FRDM-MCXN947 (Cortex-M33, ENET_QOS, FreeRTOS + lwIP) support.
End-to-end verified: NXP as CLIENT, Mac POSIX as HOST — session reaches
`Established`, UMP MIDI 2.0 notes exchanged, FEC TX/RX dropped: 0.

---

## v0.1.1 — June 2026

Initial public binary release of the AmeNote NetworkMIDI2 library.

---

## Library Features

### Protocol

- Full implementation of the **Network MIDI 2.0** session protocol
  (M2-124-UM v1.0) for both Host and Client roles.
- UDP-based UMP (Universal MIDI Packet) transport — all MIDI message types
  (MT0–MT5) supported.
- **Forward Error Correction (FEC)** — lost packets are piggybacked on
  subsequent datagrams up to a configurable FEC depth. If a gap exceeds
  that depth, a `RetransmitRequest` is issued; the sender replays from its
  TX history. Sessions survive transient WiFi packet loss without
  interruption.
- **Graceful session teardown** via bye-packet exchange (`PendingBye` state),
  notifying the peer before disconnecting.
- Keepalive ping when no UMP data is flowing (default 10 s interval).
- Inactivity timeout with automatic session reset (default 30 s).

### Authentication

- **SHA-256 shared-secret challenge-response** via `SharedSecretAuthenticator`.
  The host issues a random nonce; the client responds with `HMAC-SHA256(nonce,
  secret)`. Mismatched credentials reject the session before it reaches
  `Established`.
- Extensible via `IAuthenticator` — implement your own authentication
  strategy (per-user passwords, hardware tokens, etc.) by deriving from
  the interface in `include/networkmidi2/Authenticator.h`.

### mDNS Discovery

- Hosts can advertise using the `_midi2._udp` service type.
- Clients can discover hosts by name without knowing the IP address.
- **POSIX:** uses the system `dns_sd` / Avahi layer (`PosixMdnsDiscovery`).
- **lwIP (Pico):** uses lwIP's built-in mDNS responder (`LwipMdnsDiscovery`).

### API

- Poll-driven design — no internal threads. `tick()` drives all activity
  from the application's loop or a single dedicated task.
- `sendUmp()` / `onUmp` callback for sending and receiving UMP word arrays.
- `onStateChange` callback for all session state transitions.
- `remoteEp()` / `remoteEpName()` — query peer address and name when
  established.
- `close()` — initiates graceful teardown; poll `state()` until `Idle`.

---

## Transports Provided

| Transport | Header | Platforms |
|---|---|---|
| POSIX BSD sockets | `transports/posix/PosixUdpTransport.h` | macOS, Linux |
| lwIP raw API | `transports/lwip/LwipUdpTransport.h` | Pico W, Pico 2 W |
| NXP ENET_QOS + FreeRTOS + lwIP | `transports/nxp/NxpUdpTransport.h` | NXP FRDM-MCXN947 |
| FreeRTOS-Plus-TCP (headers only) | `transports/freertos_plus_tcp/` | FreeRTOS |

The FreeRTOS-Plus-TCP transport is provided as headers for reference. No
pre-built binary is included; compile `FreeRTOSPlusTcpUdpTransport.cpp`
from your own FreeRTOS-Plus-TCP project.

---

## Platform Support

Pre-built static libraries are provided in `lib/<platform>/`:

| Platform | Directory | Notes |
|---|---|---|
| macOS arm64 (Apple Silicon) | `lib/macos/arm64/` | Native |
| macOS x86\_64 (Intel) | `lib/macos/x86_64/` | Also runs via Rosetta 2 |
| Linux x86\_64 | `lib/linux/x86_64/` | Static musl — no runtime deps |
| Linux aarch64 (64-bit ARM) | `lib/linux/aarch64/` | RPi 4/5 (64-bit OS), AWS Graviton, etc. |
| Linux armhf (32-bit ARM) | `lib/linux/armhf/` | RPi 2/3/4 (32-bit OS) |
| Pico W — RP2040 | `lib/pico/rp2040/` | Bare-metal lwIP |
| Pico 2 W — RP2350 | `lib/pico/rp2350/` | Bare-metal lwIP |
| Pico 2 W — RP2350 + FreeRTOS | `lib/pico/rp2350-rtos/` | FreeRTOS 11.1.0, threadsafe_background lwIP |
| NXP FRDM-MCXN947 (Cortex-M33) | `lib/nxp/mcxn947/` | FreeRTOS + lwIP (NO_SYS=0, ENET_QOS) |

Linux libraries are compiled with musl libc (`-static`) and carry no
glibc or shared-library dependencies. They run on any Linux distribution.

---

## Examples Included

Source code and pre-compiled binaries for the `midi_bridge` example are
provided for all supported platforms.

### POSIX Examples (`bin/<platform>/`, source: `examples/midi_bridge/`)

| Binary | Description |
|---|---|
| `nm2_host` | Starts a Network MIDI 2.0 host, waits for a client, then sends and receives UMP MIDI in a loop |
| `nm2_client` | Connects to a host by IP address, then sends and receives UMP MIDI |
| `nm2_interactive` | Host-side interactive demo with keyboard-driven MIDI input |

Run with `--help` for usage. Key options:

```
nm2_host   [--port <n>] [--secret <passphrase>] [--advertise]
nm2_client [--host <ip>] [--port <n>] [--local <port>] [--secret <passphrase>] [--discover]
```

Defaults: port 5004, no authentication, no mDNS. Both `--advertise` and
`--discover` require mDNS support on the host OS (Bonjour on macOS, Avahi
on Linux — not available in the static Linux binaries).

### NXP Example (`bin/nxp/mcxn947/`, source: `examples/midi_bridge/nxp/`)

| Binary | Description |
|---|---|
| `nm2_nxp_mcxn947` | FRDM-MCXN947 FreeRTOS example — interactive CLI, DHCP, mDNS, MIDI bridge |

Flash with pyocd:
```bash
pyocd flash --target mcxn947 --format elf bin/nxp/mcxn947/nm2_nxp_mcxn947
```

Connect to the MCU-Link virtual COM port (LPUART4, 115200 baud). The board
prompts for mDNS name, role, and optional authentication passphrase. mDNS
auto-discovers POSIX peers on the same LAN; a numbered selection list is shown.

### Pico Examples (`bin/pico/`, source: `examples/midi_bridge/lwip/` and `freertos/`)

| Binary | Description |
|---|---|
| `bin/pico/rp2040/nm2_pico.uf2` | RP2040 Pico W — bare-metal lwIP |
| `bin/pico/rp2350/nm2_pico.uf2` | RP2350 Pico 2 W — bare-metal lwIP |
| `bin/pico/rp2350-rtos/nm2_pico_rtos.uf2` | RP2350 Pico 2 W — FreeRTOS |

Flash by holding BOOTSEL and dragging the `.uf2` file onto the Pico's
USB mass-storage drive. Connect via USB CDC serial (115200 baud) — the
example prompts for WiFi credentials (saved to flash after first entry),
mDNS name, and role (Host or Client). The FreeRTOS example additionally
supports on-demand packet-drop simulation (`d`, `D`, `r` keys) to exercise
the FEC retransmit path.

### Building Examples from Source

The example CMakeLists.txt files build against the pre-built libs in this
distribution — no NetworkMIDI2 source is required.

**NXP FRDM-MCXN947:**
```bash
cmake -B build_nxp \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-nxp-mcxn947.cmake \
      -DMCUX_SDK_PATH=/path/to/sdk \
      examples/midi_bridge/nxp
cmake --build build_nxp
pyocd flash --target mcxn947 --format elf build_nxp/nm2_nxp_mcxn947
```

**POSIX (macOS arm64):**
```bash
cmake -B build_examples/macos-arm64 examples/midi_bridge
cmake --build build_examples/macos-arm64 --parallel
```

**POSIX (Linux x86\_64, cross-compile):**
```bash
cmake -B build_examples/linux-x86_64 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-x86_64.cmake \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
      examples/midi_bridge
cmake --build build_examples/linux-x86_64 --parallel
```

**Pico 2 W bare-metal lwIP:**
```bash
cmake -B build_pico/rp2350 \
      -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0 \
      -DPICO_BOARD=pico2_w \
      examples/midi_bridge/lwip
cmake --build build_pico/rp2350 -j4
# Flash: build_pico/rp2350/nm2_pico.uf2
```

**Pico 2 W FreeRTOS:**
```bash
cmake -B build_pico/rp2350-rtos \
      -DPICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0 \
      -DPICO_BOARD=pico2_w \
      examples/midi_bridge/freertos
cmake --build build_pico/rp2350-rtos -j4
# Flash: build_pico/rp2350-rtos/nm2_pico_rtos.uf2
```

FreeRTOS-Kernel V11.1.0 is fetched automatically from GitHub via
`FetchContent` if `FREERTOS_KERNEL_PATH` is not set.

---

## Known Issues and Limitations

### Library

1. **One client per session instance.** `NetworkMidiSession` supports a
   single active client. To handle multiple simultaneous clients, create
   separate instances on distinct UDP ports.

2. **TX FIFO is 32 messages deep.** `sendUmp()` returns `false` when the
   queue is full. High-throughput applications should check the return value
   and throttle or queue externally.

3. **IPv4 only.** IPv6 addressing is not supported in this release.

4. **No session reconnection API.** After a session reaches `Idle` following
   a timeout or `close()`, call `beginHost()` or `beginClient()` again to
   restart.

5. **`tick()` is not thread-safe.** All session API calls (`sendUmp()`,
   `close()`, state query) must occur from the same thread/task that calls
   `tick()`.

### POSIX Transport / Examples

6. **mDNS not available in pre-built Linux static binaries.** The static
   musl builds cannot load Avahi at runtime. The `--advertise` and
   `--discover` flags in the example binaries have no effect on Linux. Use
   direct IP address entry instead. Applications built from source and linked
   against a system with Avahi (`libavahi-compat-libdnssd-dev`) will have
   full mDNS support.

7. **macOS mDNS requires firewall permission.** On first run, macOS will
   prompt to allow incoming network connections for `nm2_host`. Accept to
   allow clients to connect.

### Pico Examples

8. **WiFi credentials are saved to flash.** The example saves the SSID and
   passphrase to Pico flash storage after first entry. To clear saved
   credentials, reflash the `.uf2` or call `nm_wifi_clear()` from your own
   firmware build.

9. **RP2040 (Pico W) FreeRTOS not supported in this release.** Only
   bare-metal lwIP is provided for the original Pico W. FreeRTOS is
   supported on RP2350 (Pico 2 W) only.

10. **USB CDC serial requires a connection before output appears (lwIP
    example).** The bare-metal lwIP example waits for DTR assertion before
    printing. Open the serial port with a terminal emulator before powering
    the board, or send any character after connecting to trigger the prompt.
    The FreeRTOS example sets `PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1`
    and does not have this limitation.

---

## Licensing

Free for educational, evaluation, and non-commercial development and testing
use. Commercial use requires a license from AmeNote Inc.

Visit [amenote.com](https://amenote.com) — see [LICENSE](LICENSE) for terms.
