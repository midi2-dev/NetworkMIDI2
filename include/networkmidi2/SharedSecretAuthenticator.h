/**
 * @file SharedSecretAuthenticator.h
 * @brief Shared-secret IAuthenticator implementation (SHA-256 challenge-response).
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
#include "Authenticator.h"
#include <cstdint>
#include <cstring>

namespace networkmidi2 {

/**
 * @brief Ready-to-use shared-secret authenticator.
 *
 * Both the host and client are initialised with the same passphrase (or raw
 * key bytes).  Authentication uses:
 *
 *   digest = SHA-256(nonce ‖ secret)     (M2-124-UM section 6.7)
 *
 * The host generates a fresh 16-byte nonce for every session; the client
 * computes the digest and sends it in InvitationAuth (0x02).
 *
 * Usage:
 * ```cpp
 * SharedSecretAuthenticator auth("my-passphrase");
 * session.beginHost(5004, nullptr, &auth);        // host: require auth
 * // --- or ---
 * session.beginClient(hostEp, 5005, &auth);       // client: respond to challenge
 * ```
 *
 * The secret is stored as a fixed-size byte array (up to kMaxSecretLen = 64
 * bytes); no heap allocation.
 */
class SharedSecretAuthenticator : public IAuthenticator {
public:
    /** Maximum secret length in bytes. */
    static constexpr unsigned kMaxSecretLen = 64;

    /** Construct from a null-terminated passphrase string. */
    explicit SharedSecretAuthenticator(const char *secret)
    {
        size_t len = strlen(secret);
        if (len > kMaxSecretLen) len = kMaxSecretLen;
        secretLen_ = static_cast<unsigned>(len);
        memcpy(secret_, secret, secretLen_);
    }

    /** Construct from raw bytes (e.g. a pre-hashed or binary key). */
    SharedSecretAuthenticator(const uint8_t *secret, unsigned len)
    {
        if (len > kMaxSecretLen) len = kMaxSecretLen;
        secretLen_ = len;
        memcpy(secret_, secret, secretLen_);
    }

    // Always require authentication on the host side.
    bool authRequired() const override { return true; }

    /** Verify that the client digest matches SHA-256(nonce ‖ secret). */
    bool verifyDigest(const uint8_t nonce[kNonceSize],
                      const uint8_t clientDigest[kDigestSize]) override;

    /** Compute SHA-256(nonce ‖ secret) and write into @p digestOut. */
    bool buildDigest(const uint8_t nonce[kNonceSize],
                     uint8_t digestOut[kDigestSize]) override;

private:
    uint8_t  secret_[kMaxSecretLen] = {};
    unsigned secretLen_             = 0;
};

} // namespace networkmidi2
