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
 *     it writes a record into the byte ring and then publishes rxTail_.
 *   - receive() / receiveInPlace() are the sole consumer: they read rxHead_
 *     and advance it; they never touch lwIP state, so no lock is required.
 *
 * The ring offsets are std::atomic with release (producer) / acquire
 * (consumer) ordering, so a record is fully written before the consumer can
 * see it, on either core.
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

#include <atomic>

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
    const uint8_t *receiveInPlace(UdpEndpoint &from, size_t &len)            override;
    uint32_t nowMillis()                                                      override;

    /** Datagrams received since open() — wraps at UINT32_MAX; diagnostic only. */
    uint32_t rxPackets() const { return rxPackets_; }

    /** Datagrams discarded because the RX ring was full when they arrived.
     *  Non-zero means lwIP delivered faster than the session task drained, so
     *  the loss happened here rather than on the wire or at the peer — a
     *  distinction that is otherwise invisible from either end of the link. */
    uint32_t rxDropped() const { return rxDropped_; }

    /** sendTo() cost, in CPU cycles (DWT). Diagnostic only: splits a slow
     *  session tick into "waiting for the lwIP core lock", "inside lwIP and
     *  the Ethernet driver", and everything else the caller does. */
    struct TxCost {
        uint32_t calls, fails;
        uint32_t lockCyc, lockMax;   // LOCK_TCPIP_CORE() acquisition
        uint32_t sendCyc, sendMax;   // udp_sendto(), including linkoutput
    };
    const TxCost &txCost() const { return txCost_; }
    void resetTxCost() { txCost_ = TxCost{}; }

    /** Gap between consecutive datagrams handed up by lwIP, bucketed:
     *  <50us, 50-100, 100-250, 250-500, 500us-1ms, 1-2ms, 2-5ms, >=5ms.
     *  Diagnostic only. A burst the wire delivers back to back that arrives
     *  here 0.5-1 ms apart locates the pacing below this transport (driver or
     *  lwIP), not in the session that consumes the ring. */
    static constexpr unsigned kRxGapBuckets = 8;
    const uint32_t *rxGapHist() const { return rxGapHist_; }
    void resetRxGapHist() { for (auto &b : rxGapHist_) b = 0; }

    /** Largest datagram seen, as it arrived on the wire, before any clamping
     *  to kMaxPacketBytes. Compare against kMaxPacketBytes to size the buffer
     *  for the peers actually in use. */
    uint32_t rxMaxLen() const { return rxMaxLen_; }

    /** Datagrams that arrived larger than kMaxPacketBytes and were therefore
     *  clamped. Every byte past the limit is discarded, taking whole commands
     *  with it, so this must be zero for the stream to be intact. */
    uint32_t rxTruncated() const { return rxTruncated_; }

    /** Hook invoked once per datagram, immediately after it lands in the RX
     *  ring, so a polling consumer can be woken instead of waiting out its
     *  next timer tick. Runs in lwIP's tcpip thread, NOT an ISR: keep it to a
     *  task notification or a semaphore give, never real work.
     *
     *  Additive -- the transport behaves exactly as before when no hook is
     *  installed (the default), so existing users are unaffected. */
    using RxWakeupFn = void (*)(void);
    static void setRxWakeup(RxWakeupFn fn) { rxWakeup_ = fn; }

private:
    struct udp_pcb *pcb_ = nullptr;

    // The lwIP-to-session handoff is a BYTE ring of variable-size records:
    // an RxRecord header, then the datagram, padded to 4 bytes. It used to be
    // fixed slots of kMaxPacketBytes each, which held only NM2_NXP_RX_DEPTH
    // datagrams however small they were. macOS and Windows send one new UMP
    // Data command per datagram -- a few hundred bytes each -- and at 4096 B
    // SysEx the 64 x 1 KB slots overflowed in bursts: 224-246 datagrams
    // dropped per 100 messages, 83-84 SysEx truncated, with every layer below
    // clean. The same bytes now hold several hundred such datagrams.
    //
    // A record that does not fit before the end of the buffer is placed at the
    // start; the gap is marked with kWrapMark when there is room for a header,
    // and otherwise implied (fewer than sizeof(RxRecord) bytes left).
    struct RxRecord {
        uint16_t len;     // payload bytes, or kWrapMark
        uint16_t port;
        uint32_t ipv4;
    };
    static constexpr uint16_t kWrapMark = 0xFFFF;
    static constexpr unsigned recordBytes(unsigned len)
    { return (unsigned) sizeof(RxRecord) + ((len + 3u) & ~3u); }

    // Depth of the lwIP-to-session handoff ring.
    //
    // At 8 entries this dropped ~32% of inbound datagrams under a sustained
    // SysEx stream: lwIP's tcpip_thread delivers a burst far faster than the
    // session task's 1 ms cadence drains it, and a full ring discards the new
    // packet. That loss is indistinguishable from loss on the wire at both
    // ends, so it presented as an unreliable network rather than as a buffer
    // that was too small.
    //
    // Sized to absorb a burst between consecutive drains rather than to match
    // average rate; each entry costs kMaxPacketBytes, so this is the main
    // memory/robustness trade-off in this transport.
    //
    // The depth came down from 32 when kMaxPacketBytes grew from 512 to a full
    // Ethernet payload: total buffering is about the same in bytes, but fewer
    // datagrams fit, so watch rxDropped() if a peer sends small datagrams
    // faster than the session task drains. The HOST-role build is the binding
    // constraint -- it carries the USB host stack as well and overflowed SRAM
    // at depth 16.
    // Overridable per build: the HOST-role bridge carries the USB host stack
    // as well and has only a few KB of SRAM to spare, so it cannot afford the
    // depth the DEVICE build uses. Each entry costs kMaxPacketBytes.
#ifndef NM2_NXP_RX_DEPTH
#define NM2_NXP_RX_DEPTH 16
#endif
    // NM2_NXP_RX_DEPTH now sizes the ring in worst-case (full-size) datagrams,
    // so the memory cost is what it was; small datagrams simply pack tighter.
    static constexpr unsigned kRxBytes =
        NM2_NXP_RX_DEPTH * ((unsigned) sizeof(RxRecord) + ((kMaxPacketBytes + 3u) & ~3u));
    alignas(4) uint8_t rxBuf_[kRxBytes] = {};
    std::atomic<unsigned> rxHead_{0};   // consumer byte offset (session task)
    std::atomic<unsigned> rxTail_{0};   // producer byte offset (tcpip_thread)
    // receiveInPlace() hands out a pointer into the ring; the record is only
    // released on the NEXT receive call, so the producer cannot overwrite a
    // datagram while the session is still parsing it.
    unsigned rxRelease_ = 0;
    bool     rxHeld_    = false;
    const RxRecord *peekRecord(unsigned &at);
    uint32_t rxPackets_ = 0;
    uint32_t rxDropped_ = 0;
    uint32_t rxMaxLen_  = 0;
    uint32_t rxTruncated_ = 0;
    uint32_t rxGapHist_[kRxGapBuckets] = {};
    uint32_t rxLastCyc_ = 0;
    TxCost   txCost_ = {};

    static RxWakeupFn rxWakeup_;

    static void udpRecvCb(void *arg, struct udp_pcb *pcb,
                          struct pbuf *p, const ip_addr_t *addr, u16_t port);
    void        onRecv(struct pbuf *p, const ip_addr_t *addr, u16_t port);
};

} // namespace networkmidi2
