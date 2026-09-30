/**
 * @file LwipUdpTransport.h
 * @brief IUdpTransport implementation for the lwIP raw API (Phase 7).
 *
 * The lwIP UDP receive callback is asynchronous; this implementation queues
 * received datagrams internally and drains them via IUdpTransport::receive()
 * when the session calls tick().  This mirrors the pattern used by KissBox
 * NetUMP and works in both NO_SYS=1 (main-loop) and FreeRTOS lwIP modes.
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
#include "networkmidi2/Config.h"
#include "networkmidi2/UdpTransport.h"

// lwIP headers — only available when building against the Pico SDK / lwIP tree.
#include "lwip/udp.h"

namespace networkmidi2 {

/**
 * @brief lwIP raw-API UDP transport.
 *
 * ## Integration (NO_SYS=1 / Pico SDK)
 * ```cpp
 * LwipUdpTransport transport;
 * NetworkMidiSession session(transport, info, callbacks);
 * session.beginHost(5004);
 * while (true) {
 *     cyw43_arch_poll();          // or sys_check_timeouts()
 *     session.tick();
 *     sleep_ms(1);
 * }
 * ```
 */
class LwipUdpTransport : public IUdpTransport {
public:
    LwipUdpTransport();
    ~LwipUdpTransport() override;

    LwipUdpTransport(const LwipUdpTransport &)            = delete;
    LwipUdpTransport &operator=(const LwipUdpTransport &) = delete;

    bool     open(uint16_t localPort) override;
    void     close()                  override;
    bool     sendTo(const UdpEndpoint &dst, const uint8_t *data, size_t len) override;
    size_t   receive(UdpEndpoint &from, uint8_t *buf, size_t cap)            override;
    const uint8_t *receiveInPlace(UdpEndpoint &from, size_t &len)            override;
    uint32_t nowMillis()                                                      override;

    // Total UDP datagrams received (wraps at UINT32_MAX); useful for diagnostics.
    uint32_t rxPackets() const { return rxPackets_; }

    // Datagrams discarded because the receive ring was full (wraps at
    // UINT32_MAX). Nonzero means the session is not draining fast enough for
    // the inbound datagram rate; the peer sees it as loss.
    uint32_t rxDropped() const { return rxDropped_; }

private:
    // lwIP UDP PCB and internal rx ring — filled from the udp_recv callback.
    struct udp_pcb *pcb_ = nullptr;

    // Receive ring: a byte ring of variable-length records, not fixed
    // kMaxPacketBytes slots. macOS and Windows send one new UMP Data command
    // per datagram (~100-200 bytes) and a Wi-Fi peer delivers them in bursts
    // (measured: 5,400 datagrams/s peaks). Thirty-two 1 KB slots held 32 such
    // datagrams -- a few ms of a burst -- and a full ring silently dropped the
    // rest; network -> USB delivery from a Windows peer fell to 5-30% for
    // SysEx of 256 B and up. The same bytes as records hold ~6x the datagrams.
    //
    // NM2_LWIP_RX_RING_BYTES sets the size (default 64 KB: ~400 small
    // datagrams, and still >= 64 of the largest). Single producer (the lwIP
    // receive callback), single consumer (the session); a record handed out
    // by receiveInPlace() is released on the NEXT receive call, so it stays
    // valid while the session parses it even if lwIP runs on another thread.
#ifndef NM2_LWIP_RX_RING_BYTES
#define NM2_LWIP_RX_RING_BYTES 65536u
#endif
    static constexpr uint32_t kRingBytes = NM2_LWIP_RX_RING_BYTES;
    static_assert(kRingBytes >= 4u * (kMaxPacketBytes + 16u), "RX ring too small");
    struct RecHdr {             // precedes each record's payload; 8 bytes
        uint16_t len;           // payload bytes; kWrap = skip to ring start
        uint16_t port;
        uint32_t ipv4;
    };
    static constexpr uint16_t kWrap = 0xFFFF;
    alignas(4) uint8_t ring_[kRingBytes] = {};
    volatile uint32_t rxHead_ = 0;      // consumer: next record to read
    volatile uint32_t rxTail_ = 0;      // producer: next free byte
    uint32_t pendingRelease_ = 0;       // bytes of the record last handed out
    uint32_t rxPackets_ = 0;
    uint32_t rxDropped_ = 0;

    void releasePending();
    const uint8_t *peek(UdpEndpoint &from, size_t &len);

    static void udpRecvCb(void *arg, struct udp_pcb *pcb,
                          struct pbuf *p, const ip_addr_t *addr, u16_t port);
    void        onRecv(struct pbuf *p, const ip_addr_t *addr, u16_t port);
};

} // namespace networkmidi2
