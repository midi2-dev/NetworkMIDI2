/**
 * @file UdpTransport.h
 * @brief Abstract UDP transport interface for NetworkMIDI2.
 *
 * Implement this interface for each target TCP/IP stack (POSIX, lwIP,
 * FreeRTOS-Plus-TCP, …) to make the core session code stack-agnostic.
 *
 * Copyright (c) 2026 AmeNote Inc. All rights reserved.
 *
 * Part of the AmeNote NetworkMIDI2 binary distribution.
 * Free for educational, evaluation, and non-commercial development use.
 * Commercial use requires a license — visit https://amenote.com
 * See LICENSE for full terms.
 *
 * PROVIDED AS IS, WITHOUT WARRANTY OF ANY KIND. See LICENSE for the
 * full disclaimer.
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include "Types.h"

namespace networkmidi2 {

/**
 * @brief Poll-based UDP socket abstraction.
 *
 * All addresses and port numbers are in **host byte order**.  Transport
 * implementations are responsible for converting to/from network byte order
 * before calling the underlying stack.
 *
 * ## Threading model
 * `NetworkMidiSession::tick()` drives the session and is the only caller of
 * these methods.  Implementations must be safe to call from whatever context
 * the application runs `tick()` in.  For callback-based stacks (lwIP raw API),
 * the implementation queues datagrams received on the callback into an internal
 * ring, then drains that ring when `receive()` is called.
 */
class IUdpTransport {
public:
    virtual ~IUdpTransport() = default;

    /** Bind to @p localPort on all interfaces.  Returns true on success. */
    virtual bool open(uint16_t localPort) = 0;

    /** Release the socket.  Safe to call even if not open. */
    virtual void close() = 0;

    /** Send @p len bytes to @p dst.  Returns true on success. */
    virtual bool sendTo(const UdpEndpoint &dst, const uint8_t *data, size_t len) = 0;

    /** Non-blocking receive.  Returns the number of bytes placed in @p buf
     *  (and fills @p from), or 0 if no datagram is pending.
     *  Implementations must not block. */
    virtual size_t receive(UdpEndpoint &from, uint8_t *buf, size_t cap) = 0;

    /** Monotonic millisecond counter.  Used by the session for all timers.
     *  Must not return 0 (reserved as "uninitialised"). */
    virtual uint32_t nowMillis() = 0;
};

} // namespace networkmidi2
