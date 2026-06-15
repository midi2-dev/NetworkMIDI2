/**
 * @file MidiBridgeApp.h
 * @brief Host/client event-loop wrapper for the POSIX MIDI bridge example.
 *
 * Copyright (c) 2026 AmeNote Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once
#include "networkmidi2/NetworkMidiSession.h"
#include "networkmidi2/Authenticator.h"
#include "networkmidi2/Discovery.h"
#include "PosixUdpTransport.h"
#include "DemoMidiSource.h"
#include <cstdint>

namespace nm2_example {

/**
 * @brief Event-loop wrapper that connects a NetworkMidiSession to POSIX I/O.
 *
 * Owns the PosixUdpTransport and drives the NetworkMidiSession from a tight
 * 1 ms polling loop.  While Established it feeds demo MIDI from DemoMidiSource
 * and prints every received UMP message to stdout.  Ctrl-C triggers a clean
 * Bye/ByeReply shutdown.
 *
 * Usage:
 * ```cpp
 * MidiBridgeApp app("MyHost", "SN-0001");
 * app.runHost(5004);                               // blocks until done
 * // or:
 * app.runClient(0x7F000001, 5004, 5005);           // connect to 127.0.0.1:5004
 * ```
 */
class MidiBridgeApp {
public:
    /**
     * @param name       MIDI Endpoint Name — shown to the peer during handshake
     *                   (up to ~97 bytes; typically a human-readable product name).
     * @param productId  Product Instance ID — should be a persistent, unique serial
     *                   number or UUID for the physical device.
     */
    MidiBridgeApp(const char *name, const char *productId);

    // Non-copyable (owns transport state)
    MidiBridgeApp(const MidiBridgeApp &)            = delete;
    MidiBridgeApp &operator=(const MidiBridgeApp &) = delete;

    /** Optionally attach an authenticator before calling run*().
     *  If set, the host will challenge every connecting client and the client
     *  will respond to auth challenges.  Pass nullptr to disable auth. */
    void setAuth(networkmidi2::IAuthenticator *auth) { auth_ = auth; }

    /** Optionally attach an IDiscovery before calling run*().
     *  Host: calls discovery->advertise() after beginHost().
     *  Client: calls discovery->browse(), polls nextDiscovered() until a
     *  peer is found, then connects to it.  Pass nullptr to use direct IP. */
    void setDiscovery(networkmidi2::IDiscovery *disc) { discovery_ = disc; }

    /**
     * @brief Run in Host role.  Blocks until the session closes or Ctrl-C.
     * @param port  UDP port to listen on (typically 5004 for dev/testing).
     */
    void runHost(uint16_t port);

    /**
     * @brief Run in Client role.  Blocks until the session closes or Ctrl-C.
     * @param hostIp    Host IPv4 address in host byte order (e.g. 0x7F000001 = 127.0.0.1).
     * @param hostPort  Host UDP port.
     * @param localPort Local UDP port to bind.
     */
    void runClient(uint32_t hostIp, uint16_t hostPort, uint16_t localPort);

private:
    // Common event loop, called by both runHost and runClient.
    void eventLoop(networkmidi2::NetworkMidiSession &session, const char *role);

    // -----------------------------------------------------------------------
    // Callbacks — static so they match the C-style function-pointer ABI
    // expected by NetworkMidiSession::Callbacks.  The ctx pointer carries
    // `this` back to the instance.
    // -----------------------------------------------------------------------

    /** Called from tick() for each received UMP message. */
    static void onUmp(void *ctx, const uint32_t *words, size_t wordCount);

    /** Called from tick() whenever the session state changes. */
    static void onStateChange(void *ctx, networkmidi2::SessionState newState);

    /** Decode and print a UMP message (best-effort — covers common MT4 messages). */
    static void printUmp(const char *prefix, const uint32_t *words, size_t count);

    networkmidi2::EndpointInfo       info_;
    networkmidi2::PosixUdpTransport  transport_;
    DemoMidiSource                   demoSrc_;
    networkmidi2::IAuthenticator    *auth_      = nullptr;
    networkmidi2::IDiscovery        *discovery_ = nullptr;
    const char                      *role_      = "nm2";  // set in run*() for log prefix
    networkmidi2::NetworkMidiSession *session_   = nullptr;  // valid while eventLoop() runs
};

} // namespace nm2_example
