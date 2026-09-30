/**
 * @file NxpMdnsDiscovery.h
 * @brief IDiscovery implementation for NXP MCUXpresso SDK + FreeRTOS + lwIP.
 *
 * Functionally identical to LwipMdnsDiscovery but wraps every lwIP mDNS call
 * with LOCK_TCPIP_CORE() / UNLOCK_TCPIP_CORE() so it is safe to call from any
 * FreeRTOS task when lwIP runs in NO_SYS=0 (tcpip_thread) mode.
 *
 * mDNS callbacks (onSearchResult, onAddrResolved) are invoked by tcpip_thread
 * with the core lock already held — no additional locking is needed inside them.
 *
 * ## lwipopts.h requirements
 *   #define NO_SYS                  0
 *   #define LWIP_TCPIP_CORE_LOCKING 1
 *   #define LWIP_MDNS_RESPONDER     1
 *   #define LWIP_MDNS_SEARCH        1   // for browse/client mode
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
#include "networkmidi2/Discovery.h"

#ifdef NM2_HAVE_LWIP_MDNS
#include "lwip/ip_addr.h"
#include <cstdint>
struct mdns_answer;
#endif

namespace networkmidi2 {

/**
 * @brief lwIP mDNS discovery for NXP FreeRTOS targets.
 *
 * Host mode: call advertise() after DHCP assigns an IP address.
 * Client mode: call browse() then poll nextDiscovered() every tick.
 * If browse() does not find a peer, enter the host IP manually.
 */
class NxpMdnsDiscovery : public IDiscovery {
public:
    bool advertise(uint16_t port, const EndpointInfo &info) override;
    void unadvertise()                                      override;

    bool browse()                          override;
    void stopBrowse()                      override;
    bool nextDiscovered(DiscoveredPeer &)  override;

#ifdef NM2_HAVE_LWIP_MDNS
private:
    static constexpr unsigned kQueueDepth = 4;

    EndpointInfo infoCopy_{};
    int8_t       slot_   = -1;
    bool         active_ = false;

    struct PendingPeer {
        char     hostname[256];
        char     epName[kEndpointNameMax];
        char     productId[kProductIdMax];
        uint16_t port    = 0;
        // A-record RDATA as it arrived on the wire (network byte order), when
        // the responder included one in the same answer group as the SRV --
        // see onSearchResult()'s DNS_RRTYPE_A branch for why using it beats
        // resolving the SRV target by name afterwards.
        uint32_t addrNet = 0;
        bool     hasPort = false;
        bool     hasHost = false;
        bool     hasAddr = false;
    };

    PendingPeer    pending_{};
    uint8_t        requestId_        = 0xFF;
    bool           browseActive_     = false;
    bool           browseAddedNetif_ = false;
    DiscoveredPeer queue_[kQueueDepth] = {};
    unsigned       qHead_ = 0;
    unsigned       qTail_ = 0;

    // dns_gethostbyname() for a real ".local" hostname is a genuine async
    // network round-trip (can take seconds -- e.g. a real host answering
    // several seconds later, after other session traffic settles). If a
    // SECOND service's PTR/SRV/TXT answers get processed into the single
    // shared `pending_` accumulator before the FIRST service's hostname
    // resolution completes, onAddrResolved() used to read whatever
    // port/epName/productId happened to be in `pending_` *at completion
    // time* -- silently pairing the correct IP for hostname A with the
    // port/name that actually belonged to hostname B. Confirmed live:
    // browsing while both a real host and another peer were simultaneously
    // advertising produced an entry with the real host's name and port but
    // the OTHER peer's IP. Each in-flight resolution now gets its own
    // snapshot slot instead of sharing `pending_`.
    struct PendingResolve {
        NxpMdnsDiscovery *self  = nullptr;
        char              epName[kEndpointNameMax];
        char              productId[kProductIdMax];
        uint16_t          port  = 0;
        bool              inUse = false;
    };
    static constexpr unsigned kMaxOutstandingResolves = kQueueDepth;
    PendingResolve resolves_[kMaxOutstandingResolves] = {};

    void queuePush(const DiscoveredPeer &p);

    static void onSearchResult(struct mdns_answer *answer, const char *varpart,
                                int varlen, int flags, void *arg);
    static void onAddrResolved(const char *name, const ip_addr_t *addr, void *arg);
#endif
};

} // namespace networkmidi2
