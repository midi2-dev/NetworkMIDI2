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

/** NAK reason codes, M2-124-UM v1.0.1 section 6.15, Table 25.
 *
 *  A NAK reports a problem with a received Command. Per section 6.15 it never
 *  terminates a pending or established Session -- it is a complaint, not a
 *  teardown. */
enum class NakReason : uint8_t {
    Other               = 0x00,  ///< reason is in the text message
    CommandNotSupported = 0x01,  ///< required reply to an unknown Command Code
    CommandNotExpected  = 0x02,  ///< supported, but not valid in this state
    CommandMalformed    = 0x03,  ///< missing payload or unparseable values
    BadPingReply        = 0x20,  ///< Ping Reply with an unexpected Ping Id
};

/** Bye reason codes, M2-124-UM v1.0.1 section 6.16, Table 27.
 *
 *  Three of the five values previously defined here were wrong against that
 *  table: PowerDown was 0x01 (which the spec assigns to "User terminated
 *  session"), TooManySessions was 0x02 (which the spec assigns to Power Down,
 *  while "too many opened sessions" is 0x40), and Timeout was 0x10, which is
 *  not a defined reason at all. A conformant peer receiving our timeout Bye saw
 *  an unknown reason code.
 *
 *  Names now follow the spec's own wording so a future reader can check them
 *  against Table 27 without translating. */
enum class ByeReason : uint8_t {
    // Sent by either Client or Host
    UnknownOrUndefined   = 0x00,
    UserTerminated       = 0x01,
    PowerDown            = 0x02,
    TooManyMissingPackets= 0x03,  ///< cannot recover
    Timeout              = 0x04,
    SessionNotEstablished= 0x05,
    NoPendingSession     = 0x06,
    ProtocolError        = 0x07,

    // Host -> Client only
    InvitationFailedTooManySessions = 0x40,
    InvitationAuthRejectedNoPrior   = 0x41,
    InvitationRejectedByUser        = 0x42,
    InvitationRejectedAuthFailed    = 0x43,
    InvitationRejectedNoUsername    = 0x44,
    NoMatchingAuthMethod            = 0x45,

    // Client -> Host only
    InvitationCanceled              = 0x80,

    /** @deprecated Spelling kept so existing integrations still compile.
     *  0x00 is "Unknown or Undefined" in the spec, not "Normal". */
    Normal          = UnknownOrUndefined,
    /** @deprecated Use InvitationRejectedAuthFailed. */
    AuthFailed      = InvitationRejectedAuthFailed,
};

// ---------------------------------------------------------------------------
// Role
// ---------------------------------------------------------------------------

enum class Role : uint8_t {
    Host,   ///< Listens for invitations, accepts or rejects sessions
    Client, ///< Initiates sessions by sending invitations
};

} // namespace networkmidi2
