/**
 * @file Types.h
 * @brief Common data types for NetworkMIDI2.
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
#include <cstring>
#include "Config.h"

namespace networkmidi2 {

// ---------------------------------------------------------------------------
// Network address
// ---------------------------------------------------------------------------

/** IPv4 endpoint in host byte order. */
struct UdpEndpoint {
    uint32_t ipv4 = 0; ///< IPv4 address, host byte order
    uint16_t port = 0; ///< UDP port, host byte order

    bool operator==(const UdpEndpoint &o) const { return ipv4 == o.ipv4 && port == o.port; }
    bool operator!=(const UdpEndpoint &o) const { return !(*this == o); }
    bool isValid() const { return port != 0; }
};

// ---------------------------------------------------------------------------
// Endpoint identity
// ---------------------------------------------------------------------------

/** Local endpoint identity advertised during session establishment. */
struct EndpointInfo {
    char name[kEndpointNameMax]      = {};
    char productInstanceId[kProductIdMax] = {};

    void setName(const char *s) { strncpy(name, s, kEndpointNameMax - 1); }
    void setProductId(const char *s) { strncpy(productInstanceId, s, kProductIdMax - 1); }
};

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------

/** Session state machine states (M2-124-UM section 6.1). */
enum class SessionState : uint8_t {
    Idle,               ///< No session; host is listening, client has not yet invited
    PendingInvitation,  ///< Client has sent Invitation; awaiting reply
    AuthRequired,       ///< Host requires authentication; awaiting Invitation-with-Auth
    Established,        ///< Session open; UMP data may flow
    PendingReset,       ///< Sequence loss recovery in progress
    PendingBye,         ///< Bye sent; awaiting Bye Reply
};

// ---------------------------------------------------------------------------
// Bye reason codes (M2-124-UM Table for Bye command)
// ---------------------------------------------------------------------------

enum class ByeReason : uint8_t {
    Normal          = 0x00,
    PowerDown       = 0x01,
    TooManySessions = 0x02,
    Timeout         = 0x10,
    AuthFailed      = 0x43,
};

// ---------------------------------------------------------------------------
// Role
// ---------------------------------------------------------------------------

enum class Role : uint8_t {
    Host,   ///< Listens for invitations, accepts or rejects sessions
    Client, ///< Initiates sessions by sending invitations
};

} // namespace networkmidi2
