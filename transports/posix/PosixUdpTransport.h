/**
 * @file PosixUdpTransport.h
 * @brief IUdpTransport implementation using POSIX/BSD sockets.
 *
 * Suitable for desktop development and testing on macOS or Linux.  The socket
 * is set non-blocking so that IUdpTransport::receive() returns immediately
 * when no datagram is pending.
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
#include "networkmidi2/UdpTransport.h"

namespace networkmidi2 {

/**
 * @brief POSIX UDP transport (non-blocking BSD socket).
 *
 * ## Integration notes
 * - Call `open(port)` once; it binds the socket.
 * - Call `tick()` on the session from a loop; each call drains all pending
 *   datagrams via non-blocking `recvfrom()`.
 * - `nowMillis()` uses `clock_gettime(CLOCK_MONOTONIC)`.
 */
class PosixUdpTransport : public IUdpTransport {
public:
    PosixUdpTransport();
    ~PosixUdpTransport() override;

    // Non-copyable (owns a file descriptor).
    PosixUdpTransport(const PosixUdpTransport &)            = delete;
    PosixUdpTransport &operator=(const PosixUdpTransport &) = delete;

    bool     open(uint16_t localPort) override;
    void     close()                  override;
    bool     sendTo(const UdpEndpoint &dst, const uint8_t *data, size_t len) override;
    size_t   receive(UdpEndpoint &from, uint8_t *buf, size_t cap)            override;
    uint32_t nowMillis()                                                      override;

private:
    int fd_ = -1;
};

} // namespace networkmidi2
