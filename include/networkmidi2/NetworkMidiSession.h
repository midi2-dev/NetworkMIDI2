/**
 * @file NetworkMidiSession.h
 * @brief Network MIDI 2.0 session — main public API.
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
#include "Authenticator.h"
#include "Config.h"
#include "Discovery.h"
#include "Types.h"
#include "UdpTransport.h"

namespace networkmidi2 {

/**
 * @brief One Network MIDI 2.0 session (either Host or Client role).
 *
 * ## Usage — Host
 * ```cpp
 * NetworkMidiSession session(transport, info, callbacks);
 * session.beginHost(5004);          // start listening
 * while (running) {
 *     session.tick();               // call ≈ every 1 ms
 *     vTaskDelay(1);
 * }
 * session.close();
 * ```
 *
 * ## Usage — Client
 * ```cpp
 * NetworkMidiSession session(transport, info, callbacks);
 * session.beginClient(hostEp, localPort);
 * while (running) {
 *     session.tick();
 *     vTaskDelay(1);
 * }
 * session.close();
 * ```
 *
 * ## Sending UMP
 * ```cpp
 * uint32_t noteOn[2] = {0x40903C00, 0xFFFF0000};  // MT4 Note On
 * session.sendUmp(noteOn, 2);
 * ```
 *
 * ## Receiving UMP
 * Implement `Callbacks::onUmp`; it is called from within `tick()`.
 * Keep processing time minimal — do not block or call `sendUmp()` recursively.
 */
class NetworkMidiSession {
public:
    // -----------------------------------------------------------------------
    // Callbacks
    // -----------------------------------------------------------------------

    /** Application callbacks.  All are called from within tick(). */
    struct Callbacks {
        /** Called for each received UMP message.
         *  @param ctx       User context pointer supplied at construction.
         *  @param words     UMP message words (1–4).
         *  @param wordCount Number of words in this message. */
        void (*onUmp)(void *ctx, const uint32_t *words, size_t wordCount) = nullptr;

        /** Called whenever the session state changes. */
        void (*onStateChange)(void *ctx, SessionState newState) = nullptr;

        /** User context pointer passed back verbatim to all callbacks. */
        void *ctx = nullptr;
    };

    // -----------------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------------

    /**
     * @param transport  UDP transport implementation (must outlive this object).
     * @param info       Local endpoint name and product instance ID.
     * @param callbacks  Application callbacks (may have null function pointers).
     */
    NetworkMidiSession(IUdpTransport &transport,
                       const EndpointInfo &info,
                       const Callbacks &callbacks);

    ~NetworkMidiSession();

    // Non-copyable, non-movable (holds references and timer state)
    NetworkMidiSession(const NetworkMidiSession &)            = delete;
    NetworkMidiSession &operator=(const NetworkMidiSession &) = delete;

    // -----------------------------------------------------------------------
    // Session control
    // -----------------------------------------------------------------------

    /**
     * @brief Start in Host role: open @p localPort and await Invitations.
     * @param localPort  UDP port to listen on.
     * @param discovery  Optional: advertise via mDNS.
     * @param auth       Optional: require authentication.
     */
    void beginHost(uint16_t localPort,
                   IDiscovery    *discovery = nullptr,
                   IAuthenticator *auth     = nullptr);

    /**
     * @brief Start in Client role: send Invitations to @p hostEp.
     * @param hostEp     Remote host address and port.
     * @param localPort  Local UDP port to bind.
     * @param auth       Optional: provide authentication credentials.
     */
    void beginClient(const UdpEndpoint &hostEp,
                     uint16_t           localPort,
                     IAuthenticator    *auth = nullptr);

    /** Gracefully close the session (sends Bye, waits for Bye Reply in tick). */
    void close();

    // -----------------------------------------------------------------------
    // Main loop
    // -----------------------------------------------------------------------

    /**
     * @brief Drive the session.  Call approximately every 1 ms.
     *
     * Receives all pending datagrams, advances the state machine, drains the
     * TX FIFO, and manages timers.  Callbacks are invoked synchronously.
     */
    void tick();

    /**
     * @brief Limit how many received datagrams one tick() processes.
     *
     * 0 (the default) processes everything queued, as before. A bridge that
     * must also keep a small hardware receive buffer drained -- a W5500 holds
     * ~100 frames, 20-30 ms of a Wi-Fi burst -- sets a limit and calls tick()
     * repeatedly with that drain in between, so no single tick keeps it away
     * from the hardware for long. What is not processed stays queued in the
     * transport for the next tick; nothing is dropped by the limit itself.
     */
    void setMaxDatagramsPerTick(unsigned n);

    /**
     * @brief Space outbound UMP Data datagrams at least @p ms apart.
     *
     * 0 (the default) sends whatever is queued on every tick, as before. With
     * a limit, a datagram leaves only when @p ms has passed since the last
     * one, carrying everything queued meanwhile (up to the usual batch), so a
     * burst goes out as fewer, fuller datagrams instead of one per message.
     * A message after a quiet spell still leaves at once.
     *
     * For a link that cannot take one small datagram per message: a Pico 2 W
     * sending 800-1600 SysEx/s one per datagram ran its CYW43 out of transmit
     * credits, and the driver then blocked the whole firmware on every send.
     * 1 ms caps the rate at ~1000 datagrams/s for at most ~1 ms added delay,
     * and only while messages arrive faster than that.
     */
    void setMinTxIntervalMs(unsigned ms);

    /** True if the last tick() stopped at its datagram limit with more queued. */
    bool rxBacklog() const;

    // -----------------------------------------------------------------------
    // Data transfer
    // -----------------------------------------------------------------------

    /**
     * @brief Enqueue one UMP message for transmission.
     *
     * The message is not sent immediately; `tick()` drains the TX FIFO.
     *
     * @param words     UMP words (caller must supply the correct count for the
     *                  message type — use umpWordCount() from Protocol.h).
     * @param wordCount Number of 32-bit words (1–4).
     * @return true if enqueued; false if the TX FIFO is full or the session is
     *         not in the Established state.
     */
    bool sendUmp(const uint32_t *words, size_t wordCount);

    /**
     * How many further messages sendUmp() will accept right now.
     *
     * Lets a caller that is pulling from another source — a USB FIFO, say —
     * check before it dequeues, rather than discovering the TX FIFO is full
     * only after it is holding a message it can no longer put back. Without
     * this the only options at a full FIFO are to drop the message or to stall
     * the caller; leaving it in its source queue instead lets the real
     * backpressure reach whoever is sending.
     *
     * @return free slots; 0 when full, and always 0 when the session is not
     *         Established (sendUmp() would refuse anyway).
     */
    unsigned txSpaceAvailable() const;

    /**
     * Counters for the loss-recovery machinery.
     *
     * The recovery path is the one part of the session that reacts to trouble
     * by generating more work, so when a session degrades it is the first
     * thing worth being able to see. All counters saturate rather than wrap.
     */
    struct Diagnostics {
        uint32_t retransmitReqSent = 0;  ///< gaps we asked the peer to resend
        uint32_t retransmitReqRecv = 0;  ///< gaps the peer asked us to resend
        uint32_t retransmitErrSent = 0;  ///< asked for something we no longer hold
        uint32_t retransmitErrRecv = 0;  ///< peer could not resend what we asked for
        uint32_t umpEmptySent      = 0;  ///< Zero Length UMP Data Commands we sent
                                         ///< to declare an idle period (spec 7.2.1)
        uint32_t nakSent           = 0;  ///< we objected to a Command from the peer
        uint32_t nakRecv           = 0;  ///< the peer objected to one of ours --
                                         ///< a conformance signal worth watching
        uint32_t sessionResetSent  = 0;  ///< gave up on gaps and resynchronised
        uint32_t sessionResetRecv  = 0;
        uint32_t gapsDropped       = 0;  ///< missing messages never tracked: the
                                         ///< gap table was full, so they are
                                         ///< never requested and never recovered
        uint32_t gapScanMax        = 0;  ///< widest sequence jump scanned in one
                                         ///< packet; large values mean the RX
                                         ///< path is spending real time here
        uint32_t gapsSkipped       = 0;  ///< missing commands given up on --
                                         ///< Retransmit Error, retries or hold
                                         ///< time exhausted, or the hold store
                                         ///< full -- so later data is delivered
        uint32_t umpLateDropped    = 0;  ///< a command arriving after its gap
                                         ///< was given up on; delivering it
                                         ///< would break sequence order
        uint32_t heldMax           = 0;  ///< most commands held at once
                                         ///< waiting for a gap to fill
        uint32_t sysexCutOff       = 0;  ///< SysEx a given-up gap cut through,
                                         ///< ended early with a zero-length End
        uint32_t sysexOrphans      = 0;  ///< SysEx Continue/End packets dropped
                                         ///< because their Start was lost

        // Liveness. The session declares the peer dead purely on silence, so
        // these separate "we gave up on them" from "they gave up on us" --
        // which look identical from the outside and have opposite fixes.
        uint32_t pingSent          = 0;
        uint32_t pingRecv          = 0;  ///< peer checking we are alive
        uint32_t pingReplyRecv     = 0;  ///< peer answered our check
        uint32_t byeSent           = 0;  ///< we tore the session down
        uint32_t byeRecv           = 0;  ///< the peer tore it down
        uint32_t timeouts          = 0;  ///< kTimeoutMs elapsed with no RX
        // Inbound accounting, to tell "the peer is not sending" apart from
        // "we are receiving and discarding". Both end in silence at the
        // application, and nothing else distinguishes them.
        uint32_t cmdsRecv          = 0;  ///< protocol commands parsed, all types
        uint32_t umpDataRecv       = 0;  ///< UmpData commands, before filtering
        uint32_t umpDupDropped     = 0;  ///< discarded as already-seen (FEC or
                                         ///< retransmit copies)
        uint32_t umpEmptyRecv      = 0;  ///< UmpData commands carrying no payload.
                                         ///< macOS emits these to advance the
                                         ///< sequence number; they are valid and
                                         ///< must still be sequence-tracked, so
                                         ///< this is informational, not an error
        uint32_t umpMalformed      = 0;  ///< rejected on word count or length
        uint32_t umpTruncated      = 0;  ///< a UMP message declared more words
                                         ///< than its command had left. A UMP
                                         ///< message may not be split across
                                         ///< commands or datagrams, so this is
                                         ///< a peer protocol violation, not a
                                         ///< condition to accommodate --
                                         ///< non-zero means go and find who is
                                         ///< generating it.
        uint32_t pktTrailingBytes  = 0;  ///< bytes left unconsumed at the end of
                                         ///< a datagram, i.e. a command claimed
                                         ///< a length that did not fit. The
                                         ///< remainder of that packet is lost,
                                         ///< so this should always be zero.

        uint32_t maxRxSilenceMs    = 0;  ///< longest run with nothing received
                                         ///< while Established. Approaching
                                         ///< kTimeoutMs means we are about to
                                         ///< drop a peer for being quiet, even
                                         ///< if the link is perfectly healthy.
    };

    /** Snapshot of the recovery counters. */
    Diagnostics diagnostics() const;

    // -----------------------------------------------------------------------
    // Status
    // -----------------------------------------------------------------------

    SessionState  state()      const;
    UdpEndpoint   remoteEp()   const;
    const char   *remoteEpName() const;

    /**
     * @brief Reason code from the most recent Bye received from the peer.
     *
     * A peer that refuses or tears down a session says why in the Bye's CSD1
     * byte, but that reason was previously discarded, leaving a bare
     * transition to Idle as the only symptom -- indistinguishable between
     * "host is full" (TooManySessions), "host timed out" and a normal
     * close. Retained here so a caller can report it.
     *
     * Valid only after a Bye has actually arrived; returns 0xFF before that,
     * and is not cleared when a new session starts, so read it in response to
     * a transition to Idle rather than at arbitrary times.
     */
    uint8_t lastByeReason() const;

    /**
     * @brief How many Reply-Pending replies the host sent during the current
     * client connection attempt.
     *
     * Reply-Pending is handled silently (it only defers the next invitation
     * retry), so a host that keeps answering "not yet" and a host that never
     * answers at all look identical from the outside -- both just sit in
     * PendingInvitation. This distinguishes them.
     *
     * Reset by beginClient(), so read it against the attempt that just ended.
     * Saturates rather than wrapping.
     */
    uint16_t replyPendingCount() const;

private:
    struct Impl;
    Impl *impl_;
};

} // namespace networkmidi2
