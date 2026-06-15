# MIDI Bridge Example

Three programs cover the full example suite:

| Binary              | When to use                                                                                                  |
|---------------------|--------------------------------------------------------------------------------------------------------------|
| `nm2_interactive`   | Cross-platform interactive CLI — pairs with a Pico, another desktop, or a Raspberry Pi over LAN              |
| `nm2_host`          | Scripted / automated host — no user prompts, Ctrl-C to quit                                                  |
| `nm2_client`        | Scripted / automated client — no user prompts, Ctrl-C to quit                                                |

The source files are written as an **integration tutorial** — every significant
line is explained in comments.  Start here if you're porting to a new platform.

---

## Build

From the repo root:

```sh
cmake -B build
cmake --build build --parallel
```

Executables are placed in `build/examples/midi_bridge/`.

---

## nm2_interactive — Cross-platform interactive CLI

Mirrors the Pico 2 W lwIP example UI on macOS and Linux.  Use it to test
against a Pico board, a Raspberry Pi, or another desktop.

**mDNS requirements**

| Platform | Requirement                                            |
|----------|--------------------------------------------------------|
| macOS    | mDNSResponder is always present — no extras needed     |
| Linux    | `libavahi-compat-libdnssd-dev` provides the DNS-SD API |

If NM2_HAVE_DNS_SD is not detected at configure time, mDNS is silently
disabled and you must enter the host IP manually.

**Basic flow**

```sh
# Terminal A — host (prints its IPs, advertises via mDNS)
./build/examples/midi_bridge/nm2_interactive
> Role? [H]ost / [C]lient: H

# Terminal B — client (auto-discovers or enter IP manually)
./build/examples/midi_bridge/nm2_interactive
> Role? [H]ost / [C]lient: C
> (mDNS finds the host automatically, or press Enter to type the IP)
```

Both sides exchange pentatonic MIDI 2.0 notes once Established.
Type `q` + Enter to close the session; `Y` to reconnect, `R` for a new role, `N` to quit.

**Cross-machine: Mac ↔ Raspberry Pi over LAN**

If mDNS cannot auto-discover the host (different subnet or multicast blocked),
press Enter at the mDNS prompt and enter the host IP manually.

```sh
# Mac (host)
./nm2_interactive                # note the IP shown in [Host] IP lines

# Pi (client)
./nm2_interactive                # role C → mDNS finds host, or Enter to type IP
```

**Paired with a Pico 2 W**

Flash the Pico with `build_pico8/nm2_pico.uf2`, connect it to WiFi, choose
role H (host).  Then on the Mac run `nm2_interactive` → role C.  mDNS will
discover the Pico automatically on the same LAN.

---

## Run

Open two terminal windows.

**Terminal 1 — Host** (listens on UDP port 5004):
```sh
./build/examples/midi_bridge/nm2_host
```

Expected output:
```
NetworkMIDI2 Host Example
  Endpoint Name : DemoHost
  Product ID    : DEMO-HOST-0001
  Port          : 5004
Press Ctrl-C to close the session.

[nm2-host] State → Idle
[nm2-host] Listening on UDP port 5004...
[nm2-host] State → Established  (peer: "DemoClient")
[nm2-host] ← MT4 grp0 Note-On  ch0 note=60 vel=0xFFFF
[nm2-host] ← MT4 grp0 Note-Off ch0 note=60 vel=0x0000
...
```

**Terminal 2 — Client** (connects to 127.0.0.1:5004):
```sh
./build/examples/midi_bridge/nm2_client
```

Expected output:
```
NetworkMIDI2 Client Example
  Endpoint Name : DemoClient
  Product ID    : DEMO-CLIENT-0001
  Host          : 127.0.0.1:5004
  Local port    : 5005
Press Ctrl-C to close the session.

[nm2-client] State → PendingInvitation
[nm2-client] Connecting to 127.0.0.1:5004 from local port 5005...
[nm2-client] State → Established  (peer: "DemoHost")
[nm2-client] ← MT4 grp0 Note-On  ch0 note=60 vel=0xFFFF
[nm2-client] ← MT4 grp0 Note-Off ch0 note=60 vel=0x0000
...
```

Press **Ctrl-C** in either terminal to initiate a clean Bye/ByeReply shutdown.
Both sides transition to Idle and print `Done.`

---

## Command-Line Options

### nm2_host
```
--port / -p <port>   UDP port to listen on (default: 5004)
```

### nm2_client
```
--host / -H <ip>     Host IPv4 address (default: 127.0.0.1)
--port / -p <port>   Host UDP port     (default: 5004)
--local / -l <port>  Local UDP port    (default: 5005)
```

---

## What the Demo Source Generates

`DemoMidiSource` cycles through a pentatonic scale (notes 60, 62, 64, 67, 69,
72, 69, 67, 64, 62, 60, 62 …) sending one MIDI 2.0 Channel Voice Note-On every
500 ms followed by a Note-Off 200 ms later.

Both the host and the client run their own `DemoMidiSource` independently, so
you see both halves of the bidirectional exchange in each log.

---

## Source File Map

| File                             | Purpose                                                        |
|----------------------------------|----------------------------------------------------------------|
| `posix/main_interactive.cpp`     | Interactive CLI — mirrors Pico 2 W UX, runs on macOS / Linux   |
| `posix/main_host.cpp`            | Scripted host — step-by-step tutorial comments                 |
| `posix/main_client.cpp`          | Scripted client — mirrors main_host with client differences    |
| `common/MidiBridgeApp.h/.cpp`    | Event loop, callbacks, UMP printer (used by scripted pair)     |
| `common/DemoMidiSource.h/.cpp`   | Non-blocking pentatonic MIDI note generator                    |
| `lwip/main.cpp`                  | Pico 2 W interactive example (CYW43 WiFi + lwIP polling mode)  |

---

## Adapting to Your Platform

Replace `PosixUdpTransport` with your platform's transport implementation and
replace `usleep(1000)` in `MidiBridgeApp::eventLoop()` with the appropriate
1 ms sleep for your RTOS (`vTaskDelay(1)`, `k_sleep(K_MSEC(1))`, etc.).

See [docs/INTEGRATION.md](../../docs/INTEGRATION.md) for the full porting guide.

---

## Known Issues

### WiFi AP multicast isolation (Pico 2 W — mDNS may fail)

Many consumer WiFi access points block multicast traffic between stations.
When two Pico 2 W boards are on the same AP, `<name>-host.local` queries may
never reach the host board.  Symptom: client prints `(retrying...)` indefinitely.

**Workaround:** press Enter at the "Resolving" prompt and enter the host's IP
address manually.  The host IP is printed on the serial terminal after it
connects to WiFi (`[Host] IP : x.x.x.x`).

### Session drops immediately after Established (Pico 2 W — WiFi timing)

Observed on WiFi when the round-trip ping exchange is slow relative to the
session keepalive interval.  The session reaches `Established` and then drops
back to `Idle` within a few seconds without any MIDI data exchanged.

Contributing factors:
- AP-side packet buffering adding latency to ping round-trips.
- `kPingIntervalMs` (10 s, `Config.h`) and `kTimeoutMs` (30 s) were tuned for
  reliable LAN/wired paths; increase both if your AP introduces &gt;5 s jitter.
- Firmware version mismatch: earlier builds exited `SESSION_RUN` as soon as
  the session state was `Idle`, which is the host's normal pre-client state.
  Both boards must run the same (current) `nm2_pico.uf2`.

**Workaround:** test over wired Ethernet or a direct LAN connection first to
confirm the protocol layer is working, then diagnose the AP-side latency
separately.

### mDNS limited to local subnet

DNS-SD multicast (224.0.0.251) is not forwarded across subnets or through most
routers.  When testing across different network segments, choose role C →
press Enter to bypass mDNS → enter the peer IP manually.

### Pico client cannot auto-discover a POSIX host via mDNS (open)

When a Pico 2 W runs as **client** and a Mac/Linux machine runs as **host**,
the Pico's mDNS resolution of `<name>-host.local` does not succeed — the
client prints `(retrying...)` indefinitely.

Root cause: lwIP's `dns_gethostbyname` with `LWIP_DNS_SUPPORT_MDNS_QUERIES=1`
sends an A-record query for `<name>-host.local` to `224.0.0.251:5353`.
`PosixMdnsDiscovery` uses `DNSServiceRegister` with `host=nullptr`, which
publishes the SRV record pointing at the machine's **system hostname**
(e.g. `MacBook-Pro-xxxx.local`) — not `<name>-host.local`.  lwIP's query
therefore has no matching A record to respond to.

The opposite direction — **POSIX client auto-discovers a Pico host** — works
correctly because DNS-SD browse (PTR → SRV → A) resolves the system hostname,
not a `<name>.local` alias.

**Workaround:** press Enter at the "Resolving" prompt and enter the Mac/Linux
host's IP address manually.  The host IP is shown in the `[Host] IP` lines
printed to the terminal.
