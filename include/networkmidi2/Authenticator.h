/**
 * @file Authenticator.h
 * @brief Optional authentication interface for NetworkMIDI2.
 *
 * Implement IAuthenticator to enable shared-secret or user/password
 * challenge-response authentication (M2-124-UM v1.0 section 6.7).
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
#include <cstddef>
#include <cstdint>
#include "Config.h"

namespace networkmidi2 {

/**
 * @brief Authentication strategy for a Network MIDI 2.0 session.
 *
 * The spec defines a challenge-response scheme:
 *   digest = SHA256(nonce ‖ secret)          — shared-secret mode
 *   digest = SHA256(nonce ‖ username ‖ password) — user/password mode
 *
 * Implement one of these (or both) and pass to NetworkMidiSession.
 *
 * ## Host role
 * `authRequired()` is called when an incoming Invitation arrives.
 * Return true to trigger the 0x12 challenge; false to accept without auth.
 * `verifyDigest()` is called when the 0x02 Invitation-with-Auth arrives.
 *
 * ## Client role
 * `buildDigest()` is called when a 0x12 Auth-Required reply is received.
 * The implementation computes SHA256(nonce ‖ secret) and writes to @p digest.
 */
class IAuthenticator {
public:
    virtual ~IAuthenticator() = default;

    // --- Host side ---

    /** True if this authenticator requires a challenge for incoming sessions. */
    virtual bool authRequired() const = 0;

    /** Verify a client-supplied @p digest (32 bytes) against the @p nonce
     *  (16 bytes) that was sent.  Return true if authentication succeeds. */
    virtual bool verifyDigest(const uint8_t nonce[kNonceSize],
                              const uint8_t digest[kDigestSize]) = 0;

    // --- Client side ---

    /** Compute the 32-byte response digest for a host-supplied @p nonce.
     *  Write the result into @p digestOut.  Return true on success. */
    virtual bool buildDigest(const uint8_t nonce[kNonceSize],
                             uint8_t digestOut[kDigestSize]) = 0;
};

} // namespace networkmidi2
