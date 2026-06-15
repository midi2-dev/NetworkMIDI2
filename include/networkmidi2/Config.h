/**
 * @file Config.h
 * @brief Compile-time configuration constants for NetworkMIDI2.
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

namespace networkmidi2 {

// ---------------------------------------------------------------------------
// Timing (milliseconds)
// ---------------------------------------------------------------------------

/** Inactivity interval before a session is declared lost and torn down. */
constexpr unsigned int kTimeoutMs = 30000;

/** Send a Ping if no UMP data has been exchanged for this long. */
constexpr unsigned int kPingIntervalMs = 10000;

/** Re-send an Invitation this often while waiting for a reply. */
constexpr unsigned int kInviteRetryMs = 1000;

/** Wait this long for an out-of-order packet before requesting retransmit. */
constexpr unsigned int kRecoveryWaitMs = 100;

// ---------------------------------------------------------------------------
// Forward Error Correction
// ---------------------------------------------------------------------------

/** Number of previous UMP Data Commands piggybacked on each outgoing packet.
 *  Spec recommends 2 (section 7.2.2). */
constexpr unsigned int kFecDepth = 2;

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

/** Outbound UMP message FIFO depth (number of messages, each 1–4 words). */
constexpr unsigned int kTxFifoSize = 32;

/** Recently-seen RX sequence numbers kept for duplicate suppression.
 *  Must be > kFecDepth so replayed FEC copies are reliably dropped. */
constexpr unsigned int kRxSeenSize = 8;

/** Maximum endpoint name length including null terminator (bytes). */
constexpr unsigned int kEndpointNameMax = 98;

/** Maximum Product Instance ID length including null terminator (bytes). */
constexpr unsigned int kProductIdMax = 42;

/** Maximum UDP payload size in bytes (magic + all commands + payloads). */
constexpr unsigned int kMaxPacketBytes = 512;

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

/** Number of recently-sent UMP Data Commands kept in the TX retransmit buffer.
 *  Must be > kFecDepth; larger values let the peer recover from heavier loss. */
constexpr unsigned int kRetransmitBufSize = 16;

/** Maximum number of per-sequence-number gaps tracked simultaneously for retransmit. */
constexpr unsigned int kMaxPendingGaps = 4;

/** Retry limit per missing sequence number before issuing a SessionReset. */
constexpr unsigned int kMaxRetransmitRetries = 3;

/** Size of the cryptographic nonce sent by the host in Auth-Required reply. */
constexpr unsigned int kNonceSize = 16;

/** Size of the SHA-256 digest returned by the client in Invitation-with-Auth. */
constexpr unsigned int kDigestSize = 32;

} // namespace networkmidi2
