# NetworkMIDI2

C++17 library implementing the Network MIDI 2.0 protocol (M2-124-UM v1.0)
for embedded and desktop platforms. Developed by [AmeNote Inc.](https://amenote.com).

## What's included

| Item | Location |
|---|---|
| Public header files | `include/networkmidi2/` |
| Transport headers | `transports/{posix,lwip,freertos_plus_tcp,nxp}/` |
| Pre-built static libraries | `lib/<platform>/` |
| Pre-compiled example binaries | `bin/<platform>/` |
| Example source code | `examples/midi_bridge/` |
| USB MIDI third-party deps (git submodules; NXP, Pico, and pico_w examples) | `third_party/{tusb_ump,AM_MIDI2.0Lib,tinyusb,ioLibrary_Driver}/` |
| CMake `find_package()` support | `cmake/NetworkMidi2Config.cmake` |
| Linux cross-compile toolchains | `cmake/toolchain-linux-*.cmake` |
| NXP toolchain file | `cmake/toolchain-nxp-mcxn947.cmake` |
| Pico SDK import helper | `cmake/pico_sdk_import.cmake` |
| Integration guide | `docs/INTEGRATION.md` |
| Porting guide | `docs/PORTING.md` |
| Release notes | `RELEASE_NOTES.md` |

Source code is proprietary and not included in this distribution
(`third_party/` above is public upstream code this repo depends on, not
NetworkMIDI2's own source — run `git submodule update --init --recursive`
once after cloning if you'll build the NXP, Pico, or pico_w examples from
source).

---

## Quick Start — macOS / Linux (POSIX)

### 1. CMake setup

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_midi_app CXX)

list(APPEND CMAKE_PREFIX_PATH "/path/to/networkmidi2")
find_package(NetworkMidi2 REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE NetworkMidi2::nm2_transport_posix)
```

### 2. Host (accepts MIDI connections)

```cpp
#include <networkmidi2/NetworkMidiSession.h>
#include <PosixUdpTransport.h>
using namespace networkmidi2;

PosixUdpTransport transport;

EndpointInfo info;
info.setName("My MIDI Host");
info.setProductId("com.example.my-host");

NetworkMidiSession::Callbacks cb;
cb.ctx = nullptr;
cb.onUmp = [](void*, const uint32_t* words, size_t count) {
    // handle received UMP words
};
cb.onStateChange = [](void*, SessionState s) {
    // SessionState::Established — a client is connected
};

NetworkMidiSession session(transport, info, cb);
session.beginHost(5004);

while (running) {
    session.tick();   // drive the session — call every ~1 ms
}
session.close();
```

### 3. Client (connects to a host)

```cpp
UdpEndpoint host{ 0xC0A80101, 5004 };  // 192.168.1.1:5004, host byte order
session.beginClient(host, 5005);
```

---

## Quick Start — Raspberry Pi Pico 2 W (lwIP)

### CMake setup

```cmake
list(APPEND CMAKE_PREFIX_PATH "/path/to/networkmidi2")
find_package(NetworkMidi2 REQUIRED)

# Pico SDK and lwIP must already be initialised
target_link_libraries(my_target PRIVATE
    NetworkMidi2::nm2_transport_lwip
    pico_cyw43_arch_lwip_poll
    pico_lwip_mdns
)
```

### Usage

```cpp
#include <networkmidi2/NetworkMidiSession.h>
#include <LwipUdpTransport.h>
using namespace networkmidi2;

LwipUdpTransport transport;

EndpointInfo info;
info.setName("Pico MIDI Host");
info.setProductId("com.example.pico-host");

NetworkMidiSession::Callbacks cb;
cb.onUmp = [](void*, const uint32_t* words, size_t count) { /* ... */ };

NetworkMidiSession session(transport, info, cb);
session.beginHost(5004);

while (true) {
    cyw43_arch_poll();      // bare-metal lwIP; omit for FreeRTOS background arch
    session.tick();
    sleep_ms(1);
}
```

---

## Quick Start — NXP FRDM-MCXN947 (FreeRTOS + lwIP)

### CMake setup

```cmake
list(APPEND CMAKE_PREFIX_PATH "/path/to/networkmidi2")
find_package(NetworkMidi2 REQUIRED)

# NXP SDK and FreeRTOS must already be initialised in your CMake project
target_link_libraries(my_target PRIVATE
    NetworkMidi2::nm2_transport_nxp
)
```

### Usage

```cpp
#include <networkmidi2/NetworkMidiSession.h>
#include <NxpUdpTransport.h>
using namespace networkmidi2;

NxpUdpTransport transport;

EndpointInfo info;
info.setName("NXP MIDI Client");
info.setProductId("com.example.nxp-client");

NetworkMidiSession::Callbacks cb;
cb.onUmp = [](void*, const uint32_t* words, size_t count) { /* ... */ };

NetworkMidiSession session(transport, info, cb);
UdpEndpoint host{ hostIpv4, 5004 };  // host byte order
session.beginClient(host, 5005);

// In a FreeRTOS task:
while (true) {
    session.tick();
    vTaskDelay(pdMS_TO_TICKS(1));   // 1 ms
}
```

**C++ global constructors:** The NXP SDK startup `.c` file gates
`__libc_init_array()` on `__cplusplus` (false in C TUs). Call it explicitly:

```cpp
extern "C" void __libc_init_array(void);
extern "C" void _init(void) {}
extern "C" void *__dso_handle __attribute__((weak)) = nullptr;

int main(void) {
    __libc_init_array();   // run C++ global constructors
    // ...
}
```

See `docs/INTEGRATION.md` for full details.

---

## Sending MIDI

```cpp
// UMP Message Type 4 — MIDI 2.0 Note On, group 0, ch 0, note C4, full velocity
uint32_t noteOn[2]  = { 0x40903C00, 0xFFFF0000 };
session.sendUmp(noteOn, 2);

// Note Off
uint32_t noteOff[2] = { 0x40803C00, 0x00000000 };
session.sendUmp(noteOff, 2);
```

`sendUmp()` enqueues the message; `tick()` transmits it.
Call from the same thread/task as `tick()`.

---

## Authentication

Both sides must use the same passphrase:

```cpp
#include <networkmidi2/SharedSecretAuthenticator.h>
using namespace networkmidi2;

SharedSecretAuthenticator auth("my-shared-passphrase");

// Host — require authentication from all clients
session.beginHost(5004, nullptr, &auth);

// Client — respond to the host's challenge
session.beginClient(hostEp, 5005, &auth);
```

---

## Session States

`onStateChange` reports these transitions:

| State | Meaning |
|---|---|
| `Idle` | Listening (host) or not started (client) |
| `PendingInvitation` | Client sending invitations, waiting for reply |
| `AuthRequired` | Host has challenged; client is responding |
| `Established` | Session open — UMP data is flowing |
| `PendingReset` | Sequence recovery in progress |
| `PendingBye` | Graceful teardown in progress |

---

## Supported Platforms

| Platform | Transport target | Library |
|---|---|---|
| macOS arm64 | `nm2_transport_posix` | `lib/macos/arm64/` |
| macOS x86\_64 | `nm2_transport_posix` | `lib/macos/x86_64/` |
| Linux x86\_64 | `nm2_transport_posix` | `lib/linux/x86_64/` |
| Linux aarch64 (RPi 4/5, 64-bit OS) | `nm2_transport_posix` | `lib/linux/aarch64/` |
| Linux armhf (RPi 2/3/4, 32-bit OS) | `nm2_transport_posix` | `lib/linux/armhf/` |
| Pico W (RP2040) | `nm2_transport_lwip` | `lib/pico/rp2040/` |
| Pico (RP2040) + WIZnet W5500 Ethernet | `nm2_transport_lwip` | `lib/pico/rp2040-w5500/` — NOT interchangeable with `lib/pico/rp2040/`, different `lwipopts.h` |
| Pico 2 W (RP2350, bare-metal) | `nm2_transport_lwip` | `lib/pico/rp2350/` |
| Pico 2 W (RP2350, FreeRTOS) | `nm2_transport_lwip` | `lib/pico/rp2350-rtos/` |
| Pico 2 W NetworkMIDI2 Bridge (WiFi, RP2350) | `nm2_transport_lwip` | `lib/pico/rp2350/` — reuses this library (identical `lwipopts.h`), no separate lib dir |
| NXP FRDM-MCXN947 (Cortex-M33) | `nm2_transport_nxp` | `lib/nxp/mcxn947/` |

Linux binaries are fully statically linked (musl libc) — no runtime
dependencies on the target system.

For other platforms, implement `IUdpTransport` — see `docs/PORTING.md`.

---

## Pre-compiled Examples

Ready-to-run example binaries are in `bin/<platform>/`:

| Binary | Description |
|---|---|
| `nm2_host` | Waits for a client and exchanges MIDI |
| `nm2_client` | Connects to a host and exchanges MIDI |
| `nm2_interactive` | Interactive POSIX session with keyboard MIDI input |
| `nm2_pico.uf2` | Pico lwIP example — flash via BOOTSEL |
| `nm2_pico_rtos.uf2` | Pico 2 W FreeRTOS example — flash via BOOTSEL |
| `nm2_nxp_mcxn947` | NXP FRDM-MCXN947 FreeRTOS example — flash via pyocd (ELF) |
| `w5500-evb-pico/device/nm2_bridge_pico.uf2` | W5500-EVB-Pico USB MIDI 2.0 <-> Ethernet bridge, DEVICE role — flash via BOOTSEL |
| `w5500-evb-pico/host/nm2_bridge_pico.uf2` | W5500-EVB-Pico USB MIDI 2.0 <-> Ethernet bridge, HOST role — flash via BOOTSEL |
| `pico2-w/device/nm2_bridge_pico_w.uf2` | Pico 2 W USB MIDI 2.0 <-> WiFi bridge, DEVICE role (build-time `NM2_BRIDGE_USB_ROLE=DEVICE`, the default) — flash via BOOTSEL |
| `pico2-w/host/nm2_bridge_pico_w.uf2` | Pico 2 W USB MIDI 2.0 <-> WiFi bridge, HOST role (build-time `NM2_BRIDGE_USB_ROLE=HOST`) — flash via BOOTSEL |

Example source code is in `examples/midi_bridge/` and can be built from
scratch using the included CMakeLists.txt files against the pre-built libs.
See `RELEASE_NOTES.md` for build instructions.

---

## Further Reading

- **`docs/INTEGRATION.md`** — Full API reference, state machine, mDNS, platform notes
- **`docs/PORTING.md`** — Implement `IUdpTransport` for a custom network stack
- **`RELEASE_NOTES.md`** — Version history, known issues, build instructions

---

## Licensing

This library is free for **educational, evaluation, and non-commercial
development and testing** use.

**Source Code and a Commercial License available from AmeNote Inc. Please inquire**

Visit [amenote.com](https://amenote.com) to obtain a commercial license.  
See [LICENSE](LICENSE) for full terms.
