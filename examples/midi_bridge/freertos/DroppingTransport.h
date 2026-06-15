/**
 * @file DroppingTransport.h
 * @brief IUdpTransport wrapper that can silently discard packets on demand.
 *
 * Used in the FreeRTOS example to exercise the NetworkMIDI2 FEC recovery paths:
 *
 *   scheduleTxDrop(1)  — drops 1 outbound packet; kFecDepth=2 piggybacking
 *                        recovers the gap on the receiver without retransmit.
 *   scheduleTxDrop(3)  — drops 3 outbound packets; exceeds kFecDepth, so the
 *                        receiver sends a RetransmitRequest after ~100 ms.
 *   scheduleRxDrop(1)  — drops 1 inbound packet; receiving side detects the
 *                        gap and sends its own RetransmitRequest.
 *
 * All other IUdpTransport calls are forwarded to the wrapped transport.
 * The drop counters are std::atomic so they can be safely written from a CLI
 * context and read from the send/receive paths without a mutex.
 *
 * Copyright (c) 2026 AmeNote Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once
#include "networkmidi2/UdpTransport.h"
#include <atomic>
#include <cstdint>

class DroppingTransport : public networkmidi2::IUdpTransport {
public:
    explicit DroppingTransport(networkmidi2::IUdpTransport &inner) : inner_(inner) {}

    bool     open(uint16_t port) override { return inner_.open(port); }
    void     close()             override { inner_.close(); }
    uint32_t nowMillis()         override { return inner_.nowMillis(); }

    bool sendTo(const networkmidi2::UdpEndpoint &dst,
                const uint8_t *data, size_t len) override
    {
        if (dropTx_.load(std::memory_order_relaxed) > 0) {
            dropTx_.fetch_sub(1, std::memory_order_relaxed);
            ++txDropped_;
            return true;  // silently discard — caller sees "success"
        }
        return inner_.sendTo(dst, data, len);
    }

    size_t receive(networkmidi2::UdpEndpoint &from,
                   uint8_t *buf, size_t cap) override
    {
        size_t r = inner_.receive(from, buf, cap);
        if (r > 0 && dropRx_.load(std::memory_order_relaxed) > 0) {
            dropRx_.fetch_sub(1, std::memory_order_relaxed);
            ++rxDropped_;
            return 0;  // silently discard
        }
        return r;
    }

    // Schedule N future TX packets to be dropped.
    void scheduleTxDrop(unsigned n)
    {
        dropTx_.fetch_add(n, std::memory_order_relaxed);
    }

    // Schedule N future RX packets to be dropped.
    void scheduleRxDrop(unsigned n)
    {
        dropRx_.fetch_add(n, std::memory_order_relaxed);
    }

    uint32_t txDropped() const { return txDropped_; }
    uint32_t rxDropped() const { return rxDropped_; }

private:
    networkmidi2::IUdpTransport &inner_;
    std::atomic<unsigned>        dropTx_{0};
    std::atomic<unsigned>        dropRx_{0};
    uint32_t                     txDropped_ = 0;
    uint32_t                     rxDropped_ = 0;
};
