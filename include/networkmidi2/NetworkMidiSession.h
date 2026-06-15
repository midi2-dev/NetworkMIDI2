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

    // -----------------------------------------------------------------------
    // Status
    // -----------------------------------------------------------------------

    SessionState  state()      const;
    UdpEndpoint   remoteEp()   const;
    const char   *remoteEpName() const;

private:
    struct Impl;
    Impl *impl_;
};

} // namespace networkmidi2
