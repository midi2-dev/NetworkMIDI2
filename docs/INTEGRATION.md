# Integration Guide

This guide covers integrating the NetworkMIDI2 binary library into a C++17
project, from CMake setup through sending and receiving UMP MIDI data.

---

## 1. CMake Integration

### Using `find_package` (recommended)

Point CMake at the distribution root and call `find_package`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_midi_app CXX)

list(APPEND CMAKE_PREFIX_PATH "/path/to/networkmidi2")
find_package(NetworkMidi2 REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE NetworkMidi2::nm2_transport_posix)
```

Available CMake targets:

| Target | Use for |
|---|---|
| `NetworkMidi2::networkmidi2` | Core library only — bring your own transport |
| `NetworkMidi2::nm2_transport_posix` | macOS / Linux desktop (BSD sockets + mDNS) |
| `NetworkMidi2::nm2_transport_lwip` | Pico / embedded lwIP (bare-metal or FreeRTOS) |
| `NetworkMidi2::nm2_transport_nxp` | NXP FRDM-MCXN947 (FreeRTOS + lwIP, ENET_QOS) |

Each transport target automatically pulls in `NetworkMidi2::networkmidi2`
via its `INTERFACE_LINK_LIBRARIES`.

> **FreeRTOS-Plus-TCP:** Transport headers are provided in
> `transports/freertos_plus_tcp/` for reference, but no pre-built binary
> is included for this transport. Compile `FreeRTOSPlusTcpUdpTransport.cpp`
> from your own FreeRTOS project.

### Manual (no CMake)

1. Add `include/` to your include search path.
2. Add the appropriate `transports/<platform>/` directory to your include path.
3. Link against:
   - `lib/<platform>/libnetworkmidi2.a`
   - `lib/<platform>/libnm2_transport_<platform>.a`

---

## 2. Headers

```cpp
#include <networkmidi2/NetworkMidiSession.h>        // session API — include first
#include <networkmidi2/Types.h>                     // UdpEndpoint, EndpointInfo, SessionState
#include <networkmidi2/SharedSecretAuthenticator.h> // optional — SHA-256 auth

// Choose the transport for your platform:
#include <PosixUdpTransport.h>    // macOS / Linux
#include <LwipUdpTransport.h>     // Pico / lwIP
#include <NxpUdpTransport.h>      // NXP FRDM-MCXN947 (FreeRTOS + lwIP)
```

All public symbols live in the `networkmidi2` namespace.

---

## 3. Endpoint Configuration

An `EndpointInfo` describes the local device to the remote peer during
session establishment:

```cpp
using namespace networkmidi2;

EndpointInfo info;
info.setName("My MIDI Device");          // human-readable name, up to 97 chars
info.setProductId("com.acme.midi-box");  // reverse-DNS product ID, up to 41 chars
```

The Network MIDI 2.0 specification recommends **UDP port 5004** for the
primary session port. Any available UDP port is valid.

---

## 4. Constructing a Session

```cpp
NetworkMidiSession::Callbacks cb;
cb.ctx = &myAppState;

cb.onUmp = [](void* ctx, const uint32_t* words, size_t count) {
    auto* app = static_cast<MyApp*>(ctx);
    app->handleMidi(words, count);
};

cb.onStateChange = [](void* ctx, SessionState s) {
    auto* app = static_cast<MyApp*>(ctx);
    if (s == SessionState::Established)
        app->onConnected();
    else if (s == SessionState::Idle)
        app->onDisconnected();
};

PosixUdpTransport transport;
NetworkMidiSession session(transport, info, cb);
```

`NetworkMidiSession` stores a reference to `transport` — ensure `transport`
outlives the session object.

---

## 5. Host Role

A host listens on a fixed port and accepts an incoming client invitation:

```cpp
// No auth, no mDNS
session.beginHost(5004);

// With mDNS advertisement
PosixMdnsDiscovery mdns;
session.beginHost(5004, &mdns);

// With mDNS and authentication
SharedSecretAuthenticator auth("shared-passphrase");
session.beginHost(5004, &mdns, &auth);
```

One `NetworkMidiSession` instance supports **one active client at a time**.
To accept multiple simultaneous clients, create separate instances with
distinct ports.

---

## 6. Client Role

A client initiates the session by sending invitations to a known host:

```cpp
UdpEndpoint host;
host.ipv4 = (192u << 24) | (168u << 16) | (1u << 8) | 100u;  // 192.168.1.100
host.port = 5004;

session.beginClient(host, 5005);  // 5005 = local port
```

With mDNS discovery:

```cpp
PosixMdnsDiscovery mdns;
UdpEndpoint resolved = mdns.resolve("MyHost._midi2._udp.local");
if (resolved.isValid())
    session.beginClient(resolved, 5005);
```

With authentication:

```cpp
SharedSecretAuthenticator auth("shared-passphrase");
session.beginClient(host, 5005, &auth);
```

---

## 7. The tick() Loop

`tick()` drives all session activity — it receives datagrams, advances the
state machine, drains the TX queue, and fires callbacks. Call it
approximately every millisecond:

```cpp
// POSIX / desktop
while (running) {
    session.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// Bare-metal Pico (lwIP poll mode)
while (true) {
    cyw43_arch_poll();
    session.tick();
    sleep_ms(1);
}

// FreeRTOS task
while (true) {
    session.tick();
    vTaskDelay(pdMS_TO_TICKS(1));
}
```

`tick()` is **not thread-safe**. Call it from exactly one thread or task.
Do not call `sendUmp()` or `close()` concurrently with `tick()`.

---

## 8. Sending MIDI

Build UMP (Universal MIDI Packet) words and call `sendUmp()`. Messages are
enqueued in a 32-message FIFO and transmitted on the next `tick()` call.

```cpp
// Message Type 4 — MIDI 2.0 Channel Voice
// Note On: group 0, channel 0, note C4 (0x3C), full velocity
uint32_t noteOn[2]  = { 0x40903C00, 0xFFFF0000 };
bool ok = session.sendUmp(noteOn, 2);

// Note Off
uint32_t noteOff[2] = { 0x40803C00, 0x00000000 };
session.sendUmp(noteOff, 2);

// Message Type 1 — System Real-Time: MIDI Clock (0xF8)
uint32_t clock[1]   = { 0x10F80000 };
session.sendUmp(clock, 1);
```

`sendUmp()` returns `false` if the session is not `Established` or the
TX FIFO is full. Check the return value in high-throughput scenarios.

UMP word counts by message type:

| Message Type | Words | Examples |
|---|---|---|
| 0 — Utility | 1 | NOOP, clock |
| 1 — System | 1 | MIDI Clock, Start, Stop |
| 2 — MIDI 1.0 Channel Voice | 1 | Note On/Off (7-bit velocity) |
| 3 — 64-bit Data | 2 | SysEx 8 |
| 4 — MIDI 2.0 Channel Voice | 2 | Note On/Off (16-bit velocity), CC |
| 5 — 128-bit Data | 4 | SysEx 8 (large) |

---

## 9. Receiving MIDI

UMP messages arrive through `Callbacks::onUmp`, called synchronously from
within `tick()`. Keep the handler brief — copying words to a queue for
processing outside `tick()` is recommended for anything non-trivial:

```cpp
cb.onUmp = [](void* ctx, const uint32_t* words, size_t count) {
    uint8_t msgType = (words[0] >> 28) & 0xF;
    uint8_t status  = (words[0] >> 16) & 0xFF;

    if (msgType == 4 && (status & 0xF0) == 0x90) {
        // MIDI 2.0 Note On
        uint8_t  note     = (words[0] >> 8) & 0x7F;
        uint32_t velocity = words[1] >> 16;
    }
};
```

Do not call `sendUmp()` or block inside `onUmp`.

---

## 10. Session State

Query the current state at any time:

```cpp
SessionState s = session.state();
```

When connected, retrieve the remote peer's details:

```cpp
if (session.state() == SessionState::Established) {
    UdpEndpoint  peer = session.remoteEp();       // IP + port
    const char*  name = session.remoteEpName();   // display name
}
```

### State Machine

```
Idle
 │  beginHost() ──────────────────────────────► Idle (listening)
 │  beginClient()
 ▼
PendingInvitation
 │
 ├──(host accepts, no auth)──────────────────► Established
 │
 └──(host accepts, auth required)
         ▼
     AuthRequired
         │
         ├──(auth OK)────────────────────────► Established
         │
         └──(auth fail / timeout)────────────► Idle

Established
 │  packet loss > FEC depth
 ├──────────────────────────────────────────► PendingReset ──► Established
 │
 │  close() or peer timeout
 └──────────────────────────────────────────► PendingBye ────► Idle
```

| State | Meaning |
|---|---|
| `Idle` | Waiting for a client (host) or not yet started (client) |
| `PendingInvitation` | Client is sending invitations, awaiting reply |
| `AuthRequired` | Host has issued a challenge; awaiting client response |
| `Established` | Session active — UMP data flows in both directions |
| `PendingReset` | Sequence gap detected; retransmit recovery in progress |
| `PendingBye` | Graceful teardown — bye packet exchanged |

---

## 11. Graceful Shutdown

```cpp
session.close();

// Keep ticking until the session returns to Idle
while (session.state() != SessionState::Idle) {
    session.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
```

---

## 12. Authentication

`SharedSecretAuthenticator` provides built-in SHA-256 challenge-response.
Both sides must be initialised with the **same passphrase**:

```cpp
#include <networkmidi2/SharedSecretAuthenticator.h>

SharedSecretAuthenticator auth("my-shared-secret");

session.beginHost(5004, nullptr, &auth);   // host challenges every client
session.beginClient(hostEp, 5005, &auth); // client responds to challenge
```

The authenticator object must remain valid for the lifetime of the session.

To implement a custom authentication strategy, derive from `IAuthenticator`
defined in `include/networkmidi2/Authenticator.h`.

---

## 13. mDNS Discovery

### POSIX (macOS / Linux with Avahi)

```cpp
#include <PosixMdnsDiscovery.h>
PosixMdnsDiscovery mdns;
session.beginHost(5004, &mdns);  // advertises _midi2._udp service
```

> **Note:** mDNS is not available in the pre-built Linux static binaries
> (they are statically linked against musl and do not have access to Avahi).
> When building your own application against the Linux libraries, link
> against `libavahi-compat-libdnssd` to enable mDNS.

### lwIP (Pico)

```cpp
#include <LwipMdnsDiscovery.h>
LwipMdnsDiscovery mdns;
session.beginHost(5004, &mdns);
```

The `_midi2._udp` service type is used. Clients can discover hosts by name
without knowing the host IP address.

---

## 14. Platform Notes

### Pico 2 W — FreeRTOS (`lib/pico/rp2350-rtos/`)

- Call `tick()` from a dedicated FreeRTOS task at 1 ms intervals.
- Use `pico_cyw43_arch_lwip_threadsafe_background` — lwIP runs in IRQ context,
  independent of the FreeRTOS scheduler.
- FreeRTOS port: `GCC_ARM_CM33_NTZ_NONSECURE` (RP2350, no TrustZone).
- Link also against `pico_rand` (used by `SharedSecretAuthenticator` for
  nonce generation on Pico targets).

### Pico 2 W — Bare-Metal (`lib/pico/rp2350/`)

- Use `pico_cyw43_arch_lwip_poll` and call `cyw43_arch_poll()` in your
  main loop alongside `session.tick()`.
- `lwipopts.h` must appear first on the compiler include path.

### NXP FRDM-MCXN947 (`lib/nxp/mcxn947/`)

- Uses `NO_SYS=0` with `LWIP_TCPIP_CORE_LOCKING=1`. The lwIP TCP/IP thread
  runs independently; `NxpUdpTransport` uses the same callback ring-buffer
  pattern as `LwipUdpTransport` so `receive()` is non-blocking.
- Call `tick()` from a dedicated FreeRTOS task at 1 ms intervals
  (`vTaskDelay(pdMS_TO_TICKS(1))`).
- All lwIP raw-API calls inside `NxpUdpTransport` and `NxpMdnsDiscovery` are
  guarded with `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` — the session task
  needs no explicit locking.
- **C++ global constructors:** The NXP SDK startup `.c` file gates
  `__libc_init_array()` on `__cplusplus` (false in C translation units). Call
  it explicitly at the top of `main()`:
  ```cpp
  extern "C" void __libc_init_array(void);
  extern "C" void _init(void) {}
  extern "C" void *__dso_handle __attribute__((weak)) = nullptr;
  int main(void) { __libc_init_array(); ... }
  ```
  Without this call, virtual dispatch crashes on the first vtable lookup.
- Flash via `pyocd flash --target mcxn947 --format elf <elf-path>`.
- Console: MCU-Link virtual COM port, LPUART4, 115200 baud.

### Pico W — RP2040 (`lib/pico/rp2040/`)

- Same setup as Pico 2 W bare-metal; link against `lib/pico/rp2040/`.
- FreeRTOS is not supported for RP2040 in this release.

### Linux — Static Binaries

The pre-built Linux libraries and example binaries are compiled against
musl libc with `-static`. They run on any Linux distribution without
additional runtime dependencies, but `dns_sd` / Avahi mDNS is not
available.

### Timing Budget

Session timeouts (configurable via `include/networkmidi2/Config.h`):

| Constant | Default | Meaning |
|---|---|---|
| `kTimeoutMs` | 30 000 ms | Inactivity timeout before peer is considered gone |
| `kPingIntervalMs` | 10 000 ms | Keepalive ping interval when no UMP is flowing |
| `kInviteRetryMs` | 1 000 ms | Client re-invitation interval |
| `kRecoveryWaitMs` | 100 ms | Wait before issuing a RetransmitRequest |
| `kMaxPacketBytes` | 512 | Maximum UDP payload size (bytes) |
| `kTxFifoDepth` | 32 | TX queue depth in UMP messages |
