/**
 * @file Protocol.h
 * @brief Network MIDI 2.0 packet framing: command codes, build, parse, and
 *        sequence-number helpers (M2-124-UM v1.0).
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

namespace networkmidi2 {

// ---------------------------------------------------------------------------
// Wire constants
// ---------------------------------------------------------------------------

/** Every UDP datagram begins with these four bytes (ASCII "MIDI"). */
constexpr uint32_t kMagic = 0x4D494449u;

/** Size of the 4-byte magic header at the start of every datagram. */
constexpr size_t kMagicSize = 4;

/** Size of each 4-byte command header: [code][len_words][csd_hi][csd_lo]. */
constexpr size_t kCmdHeaderSize = 4;

// ---------------------------------------------------------------------------
// Command codes (M2-124-UM v1.0, Table 8)
// ---------------------------------------------------------------------------

enum class Cmd : uint8_t {
    Invitation         = 0x01, ///< Client → Host: open session
    InvitationAuth     = 0x02, ///< Client → Host: open session with SHA-256 auth
    ReplyAccepted      = 0x10, ///< Host → Client: session accepted
    ReplyPending       = 0x11, ///< Host → Client: try again later
    ReplyAuthRequired  = 0x12, ///< Host → Client: authentication required (carries nonce)
    Ping               = 0x20, ///< Either → Either: keepalive request
    PingReply          = 0x21, ///< Either → Either: keepalive response
    RetransmitRequest  = 0x80, ///< Either → Either: request retransmission of a lost packet
    RetransmitError    = 0x81, ///< Either → Either: cannot fulfil retransmit request
    SessionReset       = 0x82, ///< Either → Either: reset sequence counters to zero
    Bye                = 0xF0, ///< Either → Either: close session
    ByeReply           = 0xF1, ///< Either → Either: acknowledge Bye
    UmpData            = 0xFF, ///< Either → Either: one UMP message
};

// ---------------------------------------------------------------------------
// Parsed command view (non-owning, points into a receive buffer)
// ---------------------------------------------------------------------------

struct ParsedCmd {
    Cmd            code       = Cmd::Bye;
    uint8_t        lenWords   = 0;     ///< payload length in 32-bit words
    uint8_t        csdHi      = 0;
    uint8_t        csdLo      = 0;
    const uint8_t *payload    = nullptr; ///< points into caller's buffer
    size_t         payloadLen = 0;       ///< lenWords * 4 bytes
};

// ---------------------------------------------------------------------------
// UMP message type → word count lookup (bits 31-28 of first word)
// ---------------------------------------------------------------------------

/** Returns the number of 32-bit words in a UMP message given its first word.
 *  Source: UMP spec, message type table. */
inline uint8_t umpWordCount(uint32_t firstWord)
{
    // One entry per 4-bit message type (0x0–0xF).
    static const uint8_t kSize[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    return kSize[firstWord >> 28];
}

// ---------------------------------------------------------------------------
// Packet building  (returns bytes written; 0 on insufficient capacity)
// ---------------------------------------------------------------------------

/** Write the 4-byte magic header. Call once per datagram before any command. */
size_t pktWriteMagic(uint8_t *buf, size_t cap);

/** Append an Invitation command (0x01). */
size_t pktAppendInvitation(uint8_t *buf, size_t offset, size_t cap,
                           const char *epName, const char *piid);

/** Append an Invitation-with-Auth command (0x02).
 *  @param digest  32-byte SHA-256 digest: SHA256(nonce ‖ secret). */
size_t pktAppendInvitationAuth(uint8_t *buf, size_t offset, size_t cap,
                               const char *epName, const char *piid,
                               const uint8_t digest[32]);

/** Append a Reply-Accepted command (0x10). */
size_t pktAppendReplyAccepted(uint8_t *buf, size_t offset, size_t cap,
                              const char *epName, const char *piid);

/** Append a Reply-Pending command (0x11). */
size_t pktAppendReplyPending(uint8_t *buf, size_t offset, size_t cap);

/** Append a Reply-Auth-Required command (0x12).
 *  @param nonce  16-byte cryptographic nonce (caller generates). */
size_t pktAppendReplyAuthRequired(uint8_t *buf, size_t offset, size_t cap,
                                  const uint8_t nonce[16]);

/** Append a Ping command (0x20). @param id  Echo identifier. */
size_t pktAppendPing(uint8_t *buf, size_t offset, size_t cap, uint32_t id);

/** Append a Ping-Reply command (0x21). @param id  Echo'd identifier from Ping. */
size_t pktAppendPingReply(uint8_t *buf, size_t offset, size_t cap, uint32_t id);

/** Append a Retransmit-Request command (0x80). */
size_t pktAppendRetransmitRequest(uint8_t *buf, size_t offset, size_t cap,
                                  uint16_t seqNum);

/** Append a Retransmit-Error command (0x81). */
size_t pktAppendRetransmitError(uint8_t *buf, size_t offset, size_t cap,
                                uint16_t seqNum);

/** Append a Session-Reset command (0x82). */
size_t pktAppendSessionReset(uint8_t *buf, size_t offset, size_t cap);

/** Append a Bye command (0xF0). */
size_t pktAppendBye(uint8_t *buf, size_t offset, size_t cap, uint8_t reason);

/** Append a Bye-Reply command (0xF1). */
size_t pktAppendByeReply(uint8_t *buf, size_t offset, size_t cap);

/** Append a UMP-Data command (0xFF) for one UMP message.
 *  @param seqNum    16-bit per-session sequence number (caller manages).
 *  @param words     UMP message words (1–4).
 *  @param wordCount Number of words. */
size_t pktAppendUmpData(uint8_t *buf, size_t offset, size_t cap,
                        uint16_t seqNum, const uint32_t *words, size_t wordCount);

// ---------------------------------------------------------------------------
// Packet parsing
// ---------------------------------------------------------------------------

/** Returns true if the buffer starts with the "MIDI" magic and is ≥ 8 bytes. */
bool pktCheckMagic(const uint8_t *buf, size_t len);

/** Advance @p offset past the next command in the buffer, filling @p out.
 *  Call repeatedly until offset >= len.
 *  @return true if a valid command was found; false if the buffer is exhausted
 *          or malformed. */
bool pktNextCmd(const uint8_t *buf, size_t len, size_t &offset, ParsedCmd &out);

/** Extract endpoint name and product instance ID from an invitation payload.
 *  Writes into caller-supplied buffers (null-terminated). */
void pktParseEpNames(const ParsedCmd &cmd, char *epName, size_t epNameCap,
                     char *piid, size_t piidCap);

/** Extract the 32-bit Ping/PingReply echo ID from a command payload. */
uint32_t pktParsePingId(const ParsedCmd &cmd);

// ---------------------------------------------------------------------------
// Sequence number arithmetic (16-bit unsigned, wraparound)
// ---------------------------------------------------------------------------

/** Increment a sequence number (wraps at 0xFFFF → 0x0000). */
inline uint16_t seqNext(uint16_t n) { return static_cast<uint16_t>(n + 1u); }

/** True if sequence number @p a is strictly after @p b (two's-complement). */
inline bool seqAfter(uint16_t a, uint16_t b)
{
    // Treats the signed difference as the comparison — correct for wrap.
    return static_cast<int16_t>(a - b) > 0;
}

/** True if sequence number @p a is strictly before @p b. */
inline bool seqBefore(uint16_t a, uint16_t b) { return seqAfter(b, a); }

/** Combine two 8-bit CSD bytes into a 16-bit sequence number (big-endian). */
inline uint16_t seqFromCsd(uint8_t hi, uint8_t lo)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}

} // namespace networkmidi2
