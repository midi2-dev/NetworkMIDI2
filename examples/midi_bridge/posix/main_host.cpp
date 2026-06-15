/**
 * @file main_host.cpp
 * @brief Network MIDI 2.0 — POSIX Host example.
 *
 * # Integration Guide — Host Role
 *
 * This file is written as a tutorial.  Every important decision is explained
 * in the comments so you can adapt the pattern to your own platform (FreeRTOS,
 * lwIP, bare-metal, etc.).
 *
 * ## What this program does
 *   1. Opens a UDP socket on the specified port and waits for a client.
 *   2. Performs the Network MIDI 2.0 handshake (Invitation → ReplyAccepted).
 *   3. Exchanges MIDI 2.0 UMP messages bidirectionally using the demo source.
 *   4. Shuts down cleanly on Ctrl-C (Bye / ByeReply).
 *
 * ## Run alongside the client example
 * ```
 *   ./nm2_host             # listens on port 5004 (default)
 *   ./nm2_client           # connects to 127.0.0.1:5004
 * ```
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

#include "MidiBridgeApp.h"
#include "networkmidi2/SharedSecretAuthenticator.h"
#include "PosixMdnsDiscovery.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

// Network MIDI 2.0 has no fixed port; the host advertises its port via mDNS.
// For development without mDNS, we hard-code a well-known port here.
// The client must use the same value (or you can pass both as command-line args).
static constexpr uint16_t kDefaultPort = 5004;

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    uint16_t    port      = kDefaultPort;
    const char *secret    = nullptr;  // nullptr → no authentication
    bool        advertise = false;    // --advertise → publish via mDNS

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            port = static_cast<uint16_t>(strtoul(argv[++i], nullptr, 10));
        } else if ((strcmp(argv[i], "--secret") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc) {
            secret = argv[++i];
        } else if (strcmp(argv[i], "--advertise") == 0 || strcmp(argv[i], "-a") == 0) {
            advertise = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: nm2_host [--port <port>] [--secret <passphrase>] [--advertise]\n");
            printf("  Defaults: port=%u, no authentication, no mDNS\n", kDefaultPort);
            printf("  --advertise / -a  : publish service via mDNS (requires Bonjour / Avahi)\n");
            return 0;
        }
    }

    // -----------------------------------------------------------------------
    // Step 1 — Identify your endpoint.
    //
    // EndpointInfo carries two strings:
    //   • Endpoint Name    — human-readable device name shown to users.
    //   • Product Instance ID — persistent unique identity for this physical
    //     device (treat it like a serial number or UUID; stays the same across
    //     power cycles and firmware updates).
    //
    // In a product these would come from persistent storage (NVS / EEPROM).
    // Here we hard-code demo values.
    // -----------------------------------------------------------------------

    // MidiBridgeApp creates a PosixUdpTransport and a NetworkMidiSession.
    // The "DemoHost" name will appear in the client's log when it connects.
    // -----------------------------------------------------------------------
    // Authentication (optional).
    //
    // If --secret is given, the host will challenge every connecting client
    // with a 16-byte nonce.  The client must supply the same passphrase via
    // its own --secret flag, otherwise the session is rejected.
    //
    // The SharedSecretAuthenticator computes: digest = SHA-256(nonce ‖ secret).
    // -----------------------------------------------------------------------

    nm2_example::MidiBridgeApp app("DemoHost", "DEMO-HOST-0001");

    networkmidi2::SharedSecretAuthenticator auth(secret ? secret : "");
    if (secret) app.setAuth(&auth);

    networkmidi2::PosixMdnsDiscovery disc;
    if (advertise) app.setDiscovery(&disc);

    // -----------------------------------------------------------------------
    // Step 2 — Run in Host role.
    //
    // beginHost() opens the UDP socket and starts listening.  The session then
    // waits for an Invitation from a client.
    //
    // The event loop inside runHost():
    //   • Calls session.tick() every ~1 ms — receives datagrams, advances the
    //     state machine, drains the TX FIFO, and manages ping/timeout timers.
    //   • While Established, feeds demo MIDI from DemoMidiSource.
    //   • Prints every received UMP message (via the onUmp callback).
    //   • Handles Ctrl-C: calls session.close(), then ticks until Idle.
    //
    // On an RTOS you would replace usleep(1000) with vTaskDelay(1) and run
    // this loop in a dedicated FreeRTOS task.
    // -----------------------------------------------------------------------

    printf("NetworkMIDI2 Host Example\n");
    printf("  Endpoint Name : DemoHost\n");
    printf("  Product ID    : DEMO-HOST-0001\n");
    printf("  Port          : %u\n", port);
    printf("  Auth          : %s\n", secret    ? "enabled (--secret)" : "none");
    printf("  mDNS          : %s\n", advertise ? "advertising (--advertise)" : "none");
    printf("Press Ctrl-C to close the session.\n\n");

    app.runHost(port);  // blocks until the session closes
    return 0;
}
