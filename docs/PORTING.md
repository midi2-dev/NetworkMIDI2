# Porting Guide — Implementing IUdpTransport

This guide explains how to implement `IUdpTransport` for a network stack
not covered by the supplied transports, enabling NetworkMIDI2 to run on any
platform with UDP capability.

Pre-built transports included in this release:

| Header | Stack | Platforms |
|---|---|---|
| `transports/posix/PosixUdpTransport.h` | BSD sockets | macOS, Linux |
| `transports/lwip/LwipUdpTransport.h` | lwIP raw API | Pico, embedded |

Transport headers for FreeRTOS-Plus-TCP are in
`transports/freertos_plus_tcp/` for reference; no pre-built binary is
provided for that transport in this release.

---

## 1. The IUdpTransport Contract

```cpp
// include/networkmidi2/UdpTransport.h
class IUdpTransport {
public:
    virtual ~IUdpTransport() = default;

    virtual bool     open(uint16_t localPort)                             = 0;
    virtual void     close()                                              = 0;
    virtual bool     sendTo(const UdpEndpoint& dst,
                            const uint8_t* data, size_t len)             = 0;
    virtual size_t   receive(UdpEndpoint& from,
                             uint8_t* buf, size_t cap)                   = 0;
    virtual uint32_t nowMillis()                                          = 0;
};
```

All five methods are called exclusively by `NetworkMidiSession::tick()`.
Implementations must be safe to call from whatever context the application
runs `tick()` in.

All addresses and port numbers passed to and from these methods are in
**host byte order**. Convert to/from network byte order inside the
implementation.

---

## 2. Method Reference

### `bool open(uint16_t localPort)`

Allocate a UDP socket and bind it to `localPort` on all local interfaces
(`0.0.0.0:localPort`). Return `true` on success, `false` on failure.
Called once by `beginHost()` or `beginClient()`.

```cpp
bool MyTransport::open(uint16_t port) {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) return false;

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    return ::bind(sock_, (sockaddr*)&addr, sizeof(addr)) == 0;
}
```

### `void close()`

Release the socket. Must be safe to call even if `open()` was never called.

```cpp
void MyTransport::close() {
    if (sock_ >= 0) { ::close(sock_); sock_ = -1; }
}
```

### `bool sendTo(const UdpEndpoint& dst, const uint8_t* data, size_t len)`

Transmit `len` bytes to `dst`. `dst.ipv4` and `dst.port` are in host byte
order — convert before passing to your stack:

```cpp
bool MyTransport::sendTo(const UdpEndpoint& dst,
                         const uint8_t* data, size_t len) {
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(dst.ipv4);
    addr.sin_port        = htons(dst.port);

    ssize_t sent = ::sendto(sock_, data, len, 0,
                            (sockaddr*)&addr, sizeof(addr));
    return sent == static_cast<ssize_t>(len);
}
```

### `size_t receive(UdpEndpoint& from, uint8_t* buf, size_t cap)`

**Non-blocking** receive. If a datagram is available, copy up to `cap` bytes
into `buf`, fill `from` with the sender's address in host byte order, and
return the byte count. Return `0` immediately if no datagram is pending —
**never block**.

```cpp
size_t MyTransport::receive(UdpEndpoint& from, uint8_t* buf, size_t cap) {
    sockaddr_in addr{};
    socklen_t   addrLen = sizeof(addr);

    ssize_t n = ::recvfrom(sock_, buf, cap, MSG_DONTWAIT,
                           (sockaddr*)&addr, &addrLen);
    if (n <= 0) return 0;

    from.ipv4 = ntohl(addr.sin_addr.s_addr);
    from.port = ntohs(addr.sin_port);
    return static_cast<size_t>(n);
}
```

### `uint32_t nowMillis()`

Return a monotonic millisecond counter. Requirements:

- **Must never return `0`** — `0` is reserved as "uninitialised". Add `1`
  if your counter can be zero at startup.
- Must advance at approximately 1 ms/tick. A few ms of jitter is fine.
- Must not wrap sooner than ~49 days (`UINT32_MAX` ms).

```cpp
// POSIX
uint32_t MyTransport::nowMillis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t ms = static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return ms ? ms : 1;
}

// Pico SDK
uint32_t MyTransport::nowMillis() {
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    return ms ? ms : 1;
}

// FreeRTOS
uint32_t MyTransport::nowMillis() {
    uint32_t ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    return ms ? ms : 1;
}
```

---

## 3. Implementation Template

```cpp
// MyTransport.h
#pragma once
#include <networkmidi2/UdpTransport.h>

class MyTransport : public networkmidi2::IUdpTransport {
public:
    bool     open(uint16_t localPort)                              override;
    void     close()                                               override;
    bool     sendTo(const networkmidi2::UdpEndpoint& dst,
                    const uint8_t* data, size_t len)              override;
    size_t   receive(networkmidi2::UdpEndpoint& from,
                     uint8_t* buf, size_t cap)                    override;
    uint32_t nowMillis()                                           override;

private:
    int sock_ = -1;
};
```

---

## 4. Callback-Based (IRQ / Interrupt) Stacks

Some stacks (lwIP raw API, FreeRTOS-Plus-TCP) deliver received datagrams
via a callback rather than a blocking call. In these cases `receive()` must
drain a queue that the callback fills.

```cpp
struct RxEntry {
    networkmidi2::UdpEndpoint from;
    uint8_t data[512];
    size_t  len;
};

static constexpr size_t kRingSize = 8;
RxEntry rxRing_[kRingSize];
volatile size_t rxHead_ = 0;  // written by callback
size_t          rxTail_ = 0;  // read by receive()

// Called from stack receive callback (possibly in IRQ/task context)
void onUdpReceived(const uint8_t* data, size_t len,
                   uint32_t srcIp, uint16_t srcPort) {
    size_t next = (rxHead_ + 1) % kRingSize;
    if (next == rxTail_) return;     // ring full — drop

    RxEntry& e  = rxRing_[rxHead_];
    e.from.ipv4 = srcIp;             // already host byte order from your stack
    e.from.port = srcPort;
    e.len       = (len < sizeof(e.data)) ? len : sizeof(e.data);
    memcpy(e.data, data, e.len);
    rxHead_     = next;
}

size_t MyTransport::receive(networkmidi2::UdpEndpoint& from,
                            uint8_t* buf, size_t cap) {
    if (rxTail_ == rxHead_) return 0;
    RxEntry& e  = rxRing_[rxTail_];
    size_t   n  = (e.len < cap) ? e.len : cap;
    memcpy(buf, e.data, n);
    from    = e.from;
    rxTail_ = (rxTail_ + 1) % kRingSize;
    return n;
}
```

In FreeRTOS, protect `rxHead_` updates with `taskENTER_CRITICAL()` /
`taskEXIT_CRITICAL()` if the callback runs from a task context, or use
a `QueueHandle_t`.

---

## 5. Verifying Your Transport

Before connecting to `NetworkMidiSession`, test each method independently:

1. **open / close** — Call `open(5004)`, verify the port is bound with
   `netstat -anu`, then call `close()` and confirm the port is released.

2. **sendTo** — Call `open()`, then `sendTo()` to a known destination.
   Capture with `tcpdump -i any udp port 5004` and verify the payload
   arrives at the correct address.

3. **receive** — Send a UDP datagram to your bound port externally
   (e.g. `echo test | nc -u 127.0.0.1 5004`). Call `receive()` in a
   tight loop and confirm it returns the datagram with the correct `from`.

4. **nowMillis** — Call `nowMillis()` twice with a 100 ms delay. Confirm
   the difference is approximately 100 and that it never returns `0`.

5. **Byte order** — Confirm that a `UdpEndpoint` with
   `ipv4 = 0xC0A80101` routes to `192.168.1.1` on the wire.

---

## 6. Thread Safety

All `IUdpTransport` methods are called from the thread that calls `tick()`.
If your platform's network stack uses its own thread or IRQ, use the
ring-buffer pattern above to decouple the two execution contexts.
Never call `tick()` from multiple threads simultaneously.

---

## 7. Packet Size Limit

The session never sends or expects to receive packets larger than
`kMaxPacketBytes` (512 bytes, in `include/networkmidi2/Config.h`). Size
your receive buffer accordingly. Datagrams exceeding this limit are
silently discarded.
