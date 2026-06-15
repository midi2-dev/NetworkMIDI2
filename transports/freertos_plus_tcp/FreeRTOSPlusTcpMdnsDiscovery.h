/**
 * @file FreeRTOSPlusTcpMdnsDiscovery.h
 * @brief IDiscovery stub for FreeRTOS-Plus-TCP (Phase 8).
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

namespace networkmidi2 {

/** Placeholder mDNS discovery for FreeRTOS-Plus-TCP — Phase 8 / Phase 6 work. */
class FreeRTOSPlusTcpMdnsDiscovery : public IDiscovery {
public:
    bool advertise(uint16_t /*port*/, const EndpointInfo & /*info*/) override { return false; }
    void unadvertise()                                                override {}
    bool browse()                                                     override { return false; }
    void stopBrowse()                                                 override {}
    bool nextDiscovered(DiscoveredPeer & /*peer*/)                    override { return false; }
};

} // namespace networkmidi2
