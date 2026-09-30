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
// M2-124-UM 7.2.3 suggests "a short duration, for example 10 milliseconds":
// long enough for a reordered packet to arrive first, short enough that data
// held behind a real loss (in-order delivery, see kHoldCmds) is not delayed
// for long. It was 100 ms, which held everything behind a gap for 100-400 ms.
constexpr unsigned int kRecoveryWaitMs = 20;

// ---------------------------------------------------------------------------
// Forward Error Correction
// ---------------------------------------------------------------------------

/** Number of previous UMP Data Commands piggybacked on each outgoing packet.
 *  Spec recommends 2 (section 7.2.2). */
constexpr unsigned int kFecDepth = 2;

/** Delay before the first Zero Length UMP Data Command of an idle period.
 *
 *  M2-124-UM v1.0.1 section 7.2.1: a sender with no UMP data **shall** send a
 *  Zero Length UMP Data Command, the first "within 300ms after the most recent
 *  UMP Data Command which had a non-zero length". 250 ms leaves margin for a
 *  loop that is a little late without breaching the limit. The spec notes a
 *  shorter interval should be used for timing-critical applications. */
constexpr unsigned int kIdleFirstMs = 250;

/** Interval for the Zero Length commands that flush the FEC window.
 *
 *  Section 7.2.2: on entering an idle period a sender using FEC should send up
 *  to kFecDepth further Zero Length packets at short intervals, so the last
 *  real UMP Data Commands get their full complement of FEC repeats rather than
 *  being stranded by the silence. The spec's worked example uses 10 ms. */
constexpr unsigned int kIdleFecFlushMs = 10;

/** How many Zero Length commands one idle period emits before stopping.
 *
 *  Section 7.2.1 requires the interval to expand and the sender to "eventually
 *  stop". With doubling from kIdleFirstMs that is roughly 15 s of decreasingly
 *  frequent keepalive, after which Ping is the liveness mechanism. Bounded
 *  because the spec explicitly warns the receiver may be battery powered and
 *  "would prefer to not consistently receive data". */
constexpr unsigned int kIdleMaxCommands = 6;

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------

/** Outbound UMP message FIFO depth (number of messages, each 1–4 words).
 *  Sized for bursts rather than steady state: a multi-kilobyte SysEx arrives
 *  as hundreds of back-to-back UMP packets, and at 32 entries the FIFO filled
 *  faster than tick() could drain it, so sendUmp() refused ~8% of a sustained
 *  stream even after packet batching. Because SysEx reassembly is
 *  all-or-nothing at the receiver, losing 8% of packets loses most large
 *  messages. */
constexpr unsigned int kTxFifoSize = 128;

/** Maximum datagrams one tick() may emit while draining the TX FIFO.
 *  tick() used to send exactly one packet per invocation, capped at
 *  kRetransmitBufSize/2 messages. The USB read loop drains up to 64 messages
 *  into the FIFO in the same 1 ms pass, so the queue filled twice as fast as
 *  it emptied and sat at its 128-entry limit under load -- a standing delay
 *  ahead of every message, plus backpressure stalls, for no gain.
 *
 *  Anything queued now leaves on the same tick it arrived: at the 1 ms USB
 *  frame rate, whatever is present goes out, and nothing goes out when the
 *  FIFO is empty. Four packets x kRetransmitBufSize/2 messages covers a
 *  completely full kTxFifoSize FIFO, so the bound is reached only if a
 *  producer outruns the loop entirely -- it exists to stop one drain pass
 *  starving the session tick, keepalives and console sharing the task, not to
 *  pace transmission. */
constexpr unsigned int kMaxPacketsPerTick = 4;

/** Recently-seen RX sequence numbers kept for duplicate suppression.
 *  Must be > kFecDepth so replayed FEC copies are reliably dropped.
 *
 *  Sized well above that minimum because a duplicate is far more damaging
 *  than it used to be: a UMP Data command can carry many UMP messages, so one
 *  command that slips past this ring replays all of them. Landing mid-SysEx,
 *  that splices extra bytes into a message being reassembled -- the receiver
 *  sees corruption with nothing missing, which is much harder to diagnose
 *  than a plain drop. At 8 entries a peer whose FEC reaches further back than
 *  ours, or which simply sends faster than we drain, wrapped the ring before
 *  the copy arrived.
 *
 *  It must also cover the *retransmit* round trip, not just FEC depth: a copy
 *  we asked for arrives one request/response later, by which time the peer has
 *  sent everything in between. At 21.6 KB/s against macOS the ring wrapped
 *  well before the answer came back, so recovered commands were re-delivered
 *  as new -- measured as 5-11 duplicate SysEx and a receive count above 100%
 *  of what was sent. Sized to cover that window with kMaxPendingGaps
 *  outstanding requests in flight. */
constexpr unsigned int kRxSeenSize = 64;

/** Maximum endpoint name length including null terminator (bytes). */
constexpr unsigned int kEndpointNameMax = 98;

/** Maximum Product Instance ID length including null terminator (bytes). */
constexpr unsigned int kProductIdMax = 42;

/** Maximum UDP payload size in bytes (magic + all commands + payloads).
 *
 *  This is a receive limit as much as a send limit, and undersizing it loses
 *  data silently: a datagram larger than this is clamped on arrival, which
 *  drops whole commands off the end. The session then sees a stream with holes
 *  in it and reports corruption, giving no hint that the buffer was the
 *  problem. macOS was measured sending 672-byte datagrams against the old
 *  512-byte limit, clamping 215 datagrams in a five-second run.
 *
 *  A full Ethernet payload (1472) would be the safe choice, but the NXP
 *  HOST-role build carries the USB host stack as well and has almost no SRAM
 *  left -- it was already at 95% before this buffer grew at all. 1024 clears
 *  the largest datagram actually observed (672 from macOS) with half again as
 *  much headroom, and NxpUdpTransport::rxTruncated() counts any datagram that
 *  exceeds it, so a peer that needs more will say so instead of silently
 *  losing commands off the end. */
constexpr unsigned int kMaxPacketBytes = 1024;

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

/** Number of recently-sent UMP Data Commands kept in the TX retransmit buffer.
 *  Must be > kFecDepth; larger values let the peer recover from heavier loss.
 *  Also bounds how many messages tick() will batch into one packet (half this
 *  value), so that every message in a lost datagram is still held here when
 *  the peer asks for it. */
constexpr unsigned int kRetransmitBufSize = 64;

/** Maximum number of per-sequence-number gaps tracked simultaneously for
 *  retransmit. Also the threshold above which the session stops trying to
 *  recover and resynchronises instead.
 *
 *  Measured against macOS at 21.6 KB/s (2026-09-16): raising this to 32 does
 *  remove the reset storm -- inbound loss arrives in contiguous runs of ~16,
 *  so at 4 essentially every real loss event skipped retransmit and reset the
 *  session. But it is NOT an improvement overall and was reverted: macOS
 *  answers most of the extra requests with RetransmitErr (75 of 115 -- it no
 *  longer holds the data), the recovered copies that do arrive are delivered
 *  again rather than deduplicated (receive counts of 104-108% of what was
 *  sent), and the usb2net direction wedged more often. Enlarging kRxSeenSize
 *  to 256 alongside it did not reduce the duplicates, so ring wrap is not the
 *  cause and the duplicate path needs finding before this is raised again.
 *
 *  Raised to 32 on 2026-09-26, once both objections were gone: a Retransmit
 *  Error no longer resets the session (the gap is skipped, 7.2.4), and with
 *  in-order delivery a recovered copy can no longer be delivered twice -- one
 *  behind the delivery position is dropped as late, one ahead of it is a
 *  duplicate. At 4, packets merely reordered by 2 ms (netem) produced jumps of
 *  up to 125 that were given up as unrecoverable, and the reordered packets
 *  then arrived late and were dropped. A jump wider than this is still held
 *  (see kMaxHoldMs); it is just not requested. */
constexpr unsigned int kMaxPendingGaps = 32;

/** Retry limit per missing sequence number before issuing a SessionReset. */
constexpr unsigned int kMaxRetransmitRetries = 3;

/** In-order delivery (M2-124-UM 5.6: the Sequence Number "declares and
 *  maintains the sequential order" of UMP). Real data that arrives ahead of
 *  a missing command is held until the gap is filled -- by an FEC copy, a
 *  retransmit, or a reordered packet -- or given up on, and is then delivered
 *  in sequence order. FEC copies of commands already processed are never held;
 *  they are dropped as duplicates first. The store is bounded; when it fills,
 *  the gap is given up on rather than letting one lost packet wedge the
 *  stream. Nothing is held on a stream with no gaps. */
constexpr unsigned int kHoldCmds  = 256;   ///< commands held ahead of a gap
constexpr unsigned int kHoldWords = 2048;  ///< UMP words across them (8 KB)
/** Longest a gap at the head of the stream may hold later data. Covers the
 *  first retransmit wait plus its retries; also resolves a gap nothing is
 *  tracking (the gap table was full). */
constexpr unsigned int kMaxHoldMs = kRecoveryWaitMs * (kMaxRetransmitRetries + 1u) + 50u;

/** Size of the cryptographic nonce sent by the host in Auth-Required reply. */
constexpr unsigned int kNonceSize = 16;

/** Size of the SHA-256 digest returned by the client in Invitation-with-Auth. */
constexpr unsigned int kDigestSize = 32;

} // namespace networkmidi2
