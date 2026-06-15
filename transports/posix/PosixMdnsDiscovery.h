/**
 * @file PosixMdnsDiscovery.h
 * @brief IDiscovery implementation using the DNS-SD API (Bonjour / Avahi).
 *
 * Requires dns_sd.h to be available.  On macOS the mDNSResponder daemon is
 * always present; on Linux install libavahi-compat-libdnssd-dev.  When
 * dns_sd.h is absent the class degrades to a harmless no-op stub (CMake sets
 * NM2_HAVE_DNS_SD=1 only when the header is found and the library links).
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

#ifdef NM2_HAVE_DNS_SD
#  include <dns_sd.h>
#endif

namespace networkmidi2 {

/**
 * @brief DNS-SD (Bonjour / Avahi compat) implementation of IDiscovery.
 *
 * ## Host — advertise:
 * ```cpp
 * PosixMdnsDiscovery disc;
 * session.beginHost(5004, &disc);   // calls disc.advertise() automatically
 * // run tick() loop; disc.unadvertise() is called on teardown
 * ```
 *
 * ## Client — browse then connect:
 * ```cpp
 * PosixMdnsDiscovery disc;
 * disc.browse();
 * DiscoveredPeer peer;
 * while (!disc.nextDiscovered(peer)) { usleep(10000); }
 * disc.stopBrowse();
 * session.beginClient(peer.endpoint, localPort);
 * ```
 *
 * All DNS-SD I/O is non-blocking (select(0) before processResult).  On macOS
 * the mDNSResponder daemon handles multicast; this class only exchanges
 * messages with it over a local Unix socket.
 */
class PosixMdnsDiscovery : public IDiscovery {
public:
    PosixMdnsDiscovery();
    ~PosixMdnsDiscovery() override;

    PosixMdnsDiscovery(const PosixMdnsDiscovery &)            = delete;
    PosixMdnsDiscovery &operator=(const PosixMdnsDiscovery &) = delete;

    /** Publish "_midi2._udp" on @p port; TXT record carries nm= and prid=. */
    bool advertise(uint16_t port, const EndpointInfo &info) override;

    /** Withdraw the advertisement and release the DNS-SD service ref. */
    void unadvertise() override;

    /** Start browsing for "_midi2._udp.local" services. */
    bool browse() override;

    /** Stop browsing and release all in-flight resolve references. */
    void stopBrowse() override;

    /** Pump the DNS-SD event loop; return the next fully-resolved peer.
     *  Returns false when the queue is empty.  Call from the tick loop. */
    bool nextDiscovered(DiscoveredPeer &peer) override;

private:
    static constexpr unsigned kMaxResolves = 4;  ///< Simultaneous resolve chains
    static constexpr unsigned kQueueDepth  = 8;  ///< Ready-to-return peer queue

#ifdef NM2_HAVE_DNS_SD
    // Tracks one in-flight browse → resolve → address chain.
    struct ResolveEntry {
        PosixMdnsDiscovery *owner      = nullptr;
        DNSServiceRef       resolveRef = nullptr;
#ifdef __APPLE__
        // Avahi compat does not provide DNSServiceGetAddrInfo; Linux uses
        // getaddrinfo() synchronously in onResolve instead.
        DNSServiceRef       addrRef    = nullptr;
#endif
        char  epName[kEndpointNameMax] = {};
        char  productId[kProductIdMax] = {};
        uint16_t port  = 0;
        uint32_t iface = 0;
        bool     valid = false;
    };

    DNSServiceRef  advertRef_              = nullptr;
    DNSServiceRef  browseRef_              = nullptr;
    ResolveEntry   resolves_[kMaxResolves] = {};
    DiscoveredPeer queue_[kQueueDepth]     = {};
    unsigned       qHead_                  = 0;
    unsigned       qTail_                  = 0;

    static void pumpRef(DNSServiceRef ref);  ///< Non-blocking select + processResult
    void        pumpAll();
    void        queuePush(const DiscoveredPeer &p);
    ResolveEntry *allocResolve();
    void          freeResolve(ResolveEntry &e);

    static void DNSSD_API onBrowse(
        DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType,
        const char *, const char *, const char *, void *);

    static void DNSSD_API onResolve(
        DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType,
        const char *, const char *, uint16_t, uint16_t,
        const unsigned char *, void *);

#ifdef __APPLE__
    static void DNSSD_API onAddrInfo(
        DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType,
        const char *, const ::sockaddr *, uint32_t, void *);
#endif  // __APPLE__
#endif  // NM2_HAVE_DNS_SD
};

} // namespace networkmidi2
