/**
 * @file LwipMdnsDiscovery.h
 * @brief IDiscovery implementation using the lwIP mDNS responder + searcher.
 *
 * When NM2_HAVE_LWIP_MDNS=1 (set by the Pico 2 W CMakeLists):
 *
 * Host mode — advertise():
 *   Registers a _midi2._udp DNS-SD service record on netif_default via
 *   the lwIP mDNS responder API (LWIP_MDNS_RESPONDER=1).
 *
 * Client mode — browse() / nextDiscovered():
 *   Sends a DNS-SD PTR query for _midi2._udp via mdns_search_service()
 *   (LWIP_MDNS_SEARCH=1).  When the response arrives the callback collects
 *   the SRV hosttarget + port, then resolves the hostname via
 *   dns_gethostbyname().  Because the SRV target is the peer's real system
 *   hostname (always has an A record), this works with both Mac and Linux.
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
// Forward-declare the lwIP search-result struct at global scope so that
// 'struct mdns_answer *' inside namespace networkmidi2 refers to ::mdns_answer,
// not a new networkmidi2::mdns_answer incomplete type.
struct mdns_answer;
#endif

namespace networkmidi2 {

/**
 * @brief lwIP mDNS responder — advertise only (no browse).
 *
 * Call advertise() after WiFi is connected and netif_default is valid.
 * The mDNS subsystem is initialised on first call and torn down by
 * unadvertise().
 */
class LwipMdnsDiscovery : public IDiscovery {
public:
    bool advertise(uint16_t port, const EndpointInfo &info) override;
    void unadvertise()                                      override;

    bool browse()                          override;
    void stopBrowse()                      override;
    bool nextDiscovered(DiscoveredPeer &)  override;

#ifdef NM2_HAVE_LWIP_MDNS
private:
    static constexpr unsigned kQueueDepth = 4;

    // Host-mode state
    EndpointInfo infoCopy_{};
    int8_t       slot_   = -1;
    bool         active_ = false;

    // Client-mode browse state: accumulates one response frame across callbacks.
    // A PTR query response normally carries PTR + SRV + TXT + (implied) A in
    // one packet; after the final answer we call dns_gethostbyname on the SRV
    // hosttarget (the peer's real system hostname, which always has an A record).
    struct PendingPeer {
        char     hostname[256];              // SRV hosttarget for dns_gethostbyname
        char     epName[kEndpointNameMax];   // from PTR label or TXT nm=
        char     productId[kProductIdMax];   // from TXT prid=
        uint16_t port    = 0;
        bool     hasPort = false;
        bool     hasHost = false;
    };

    PendingPeer    pending_{};
    uint8_t        requestId_           = 0xFF; // 0xFF = not browsing
    bool           browseActive_        = false;
    bool           browseAddedNetif_    = false; // true when browse() added the netif (not advertise())
    DiscoveredPeer queue_[kQueueDepth]  = {};
    unsigned       qHead_               = 0;
    unsigned       qTail_               = 0;

    void queuePush(const DiscoveredPeer &p);

    // DNS-SD search callback — collects PTR/SRV/TXT answers, then triggers
    // dns_gethostbyname on the SRV hosttarget.
    static void onSearchResult(struct mdns_answer *answer, const char *varpart,
                                int varlen, int flags, void *arg);

    // dns_gethostbyname callback — pushes the resolved peer to the queue.
    static void onAddrResolved(const char *name, const ip_addr_t *addr, void *arg);
#endif
};

} // namespace networkmidi2
