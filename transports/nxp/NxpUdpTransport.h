/**
 * @file NxpUdpTransport.h
 * @brief IUdpTransport implementation for NXP MCUXpresso SDK + FreeRTOS + lwIP.
 *
 * Targets the NXP FRDM-MCXN947 (MCX N947 dual Cortex-M33 + ENET_QOS Ethernet).
 * Uses the lwIP raw API in NO_SYS=0 (FreeRTOS tcpip_thread) mode with
 * LWIP_TCPIP_CORE_LOCKING=1.
 *
 * ## Thread-safety model
 * The session task calls open(), close(), sendTo(), receive(), and nowMillis()
 * from a single FreeRTOS task.  The lwIP UDP receive callback fires from
 * tcpip_thread.  The two threads share a SPSC ring buffer:
 *
 *   - open() / close() / sendTo() acquire LOCK_TCPIP_CORE() before any lwIP
 *     raw API call.  UNLOCK_TCPIP_CORE() is called on every exit path.
 *   - onRecv() is called with the core lock already held by tcpip_thread;
 *     it writes into the ring and advances rxTail_.
 *   - receive() is the sole consumer: reads rxHead_ and advances it; never
 *     touches lwIP state, so no lock is required.
 *
 * The ring indices are plain unsigned ints.  On a single-core Cortex-M33 the
 * mutex release inside UNLOCK_TCPIP_CORE() provides the necessary store-release
 * barrier so the consumer observes all ring writes before seeing the updated
 * rxTail_.  If both cores of the MCXN947 are used, wrap the indices in
 * std::atomic with acquire/release ordering.
 *
 * ## lwipopts.h requirements
 *   #define NO_SYS                  0
 *   #define LWIP_TCPIP_CORE_LOCKING 1
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

#include "lwip/udp.h"

namespace networkmidi2 {

/**
 * @brief lwIP raw-API UDP transport for NXP MCUXpresso SDK + FreeRTOS.
 *
 * ## Quick-start
 * ```cpp
 * NxpUdpTransport transport;
 * NetworkMidiSession session(transport, info, callbacks);
 *
 * // Inside FreeRTOS task, after DHCP has assigned an IP:
 * session.beginHost(5004);
 * for (;;) {
 *     session.tick();
 *     vTaskDelay(pdMS_TO_TICKS(1));
 * }
 * ```
 */
class NxpUdpTransport : public IUdpTransport {
public:
    NxpUdpTransport();
    ~NxpUdpTransport() override;

    NxpUdpTransport(const NxpUdpTransport &)            = delete;
    NxpUdpTransport &operator=(const NxpUdpTransport &) = delete;

    bool     open(uint16_t localPort) override;
    void     close()                  override;
    bool     sendTo(const UdpEndpoint &dst, const uint8_t *data, size_t len) override;
    size_t   receive(UdpEndpoint &from, uint8_t *buf, size_t cap)            override;
    uint32_t nowMillis()                                                      override;

    /** Datagrams received since open() — wraps at UINT32_MAX; diagnostic only. */
    uint32_t rxPackets() const { return rxPackets_; }

private:
    struct udp_pcb *pcb_ = nullptr;

    struct RxEntry {
        UdpEndpoint from;
        uint8_t     data[kMaxPacketBytes];
        size_t      len;
    };
    static constexpr unsigned kRxDepth = 8;
    RxEntry  rxRing_[kRxDepth] = {};
    unsigned rxHead_    = 0;   // consumer index (session task)
    unsigned rxTail_    = 0;   // producer index (tcpip_thread callback)
    uint32_t rxPackets_ = 0;

    static void udpRecvCb(void *arg, struct udp_pcb *pcb,
                          struct pbuf *p, const ip_addr_t *addr, u16_t port);
    void        onRecv(struct pbuf *p, const ip_addr_t *addr, u16_t port);
};

} // namespace networkmidi2
