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
    uint32_t nowMillis()                                                      override;

    // Total UDP datagrams received (wraps at UINT32_MAX); useful for diagnostics.
    uint32_t rxPackets() const { return rxPackets_; }

private:
    // lwIP UDP PCB and internal rx ring — filled from the udp_recv callback.
    struct udp_pcb *pcb_ = nullptr;

    struct RxEntry {
        UdpEndpoint from;
        uint8_t     data[kMaxPacketBytes];
        size_t      len;
    };
    static constexpr unsigned kRxDepth = 8;
    RxEntry  rxRing_[kRxDepth] = {};
    unsigned rxHead_  = 0;
    unsigned rxTail_  = 0;
    uint32_t rxPackets_ = 0;

    static void udpRecvCb(void *arg, struct udp_pcb *pcb,
                          struct pbuf *p, const ip_addr_t *addr, u16_t port);
    void        onRecv(struct pbuf *p, const ip_addr_t *addr, u16_t port);
};

} // namespace networkmidi2
