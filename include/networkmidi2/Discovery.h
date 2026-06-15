/**
 * @file Discovery.h
 * @brief Optional mDNS/DNS-SD discovery interface for NetworkMIDI2.
 *
 * The spec service type is "_midi2._udp" (local: "_midi2._udp.local").
 * Discovery is optional; devices may connect by direct IP:port instead.
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
#include <cstdint>
#include "Types.h"

namespace networkmidi2 {

/** DNS-SD service type for Network MIDI 2.0. */
constexpr const char *kMdnsServiceType = "_midi2._udp";

/** Discovered peer record returned by IDiscovery::nextDiscovered(). */
struct DiscoveredPeer {
    UdpEndpoint  endpoint;
    char         epName[kEndpointNameMax]  = {};
    char         productId[kProductIdMax]  = {};
};

/**
 * @brief Optional mDNS/DNS-SD service advertisement and discovery.
 *
 * ## Host role
 * Call `advertise()` after `beginHost()` to publish the "_midi2._udp" service.
 *
 * ## Client role
 * Call `browse()` to start background discovery.  Poll `nextDiscovered()` each
 * `tick()` and pass the returned endpoint to `beginClient()`.
 */
class IDiscovery {
public:
    virtual ~IDiscovery() = default;

    /** Advertise this device as a Network MIDI 2.0 host on @p port. */
    virtual bool advertise(uint16_t port, const EndpointInfo &info) = 0;

    /** Stop advertising. */
    virtual void unadvertise() = 0;

    /** Start browsing for "_midi2._udp" services. */
    virtual bool browse() = 0;

    /** Stop browsing. */
    virtual void stopBrowse() = 0;

    /** Returns true and fills @p peer if a new peer was discovered since the
     *  last call.  Returns false if the discovery queue is empty. */
    virtual bool nextDiscovered(DiscoveredPeer &peer) = 0;
};

} // namespace networkmidi2
