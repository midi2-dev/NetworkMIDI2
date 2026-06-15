/**
 * @file main_client.cpp
 * @brief Network MIDI 2.0 — POSIX Client example.
 *
 * # Integration Guide — Client Role
 *
 * This file is the client counterpart to main_host.cpp.  Read that file first
 * for the full narrative; this file highlights the differences.
 *
 * ## What this program does
 *   1. Sends an Invitation to the host at the specified IP:port.
 *   2. Retries every kInviteRetryMs until ReplyAccepted arrives.
 *   3. Exchanges UMP messages bidirectionally while Established.
 *   4. Shuts down cleanly on Ctrl-C.
 *
 * ## Run alongside the host example
 * ```
 *   ./nm2_host             # terminal 1 — listens on port 5004
 *   ./nm2_client           # terminal 2 — connects to 127.0.0.1:5004
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
#include <arpa/inet.h>    // inet_pton
#include <netinet/in.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Defaults — must match nm2_host
// ---------------------------------------------------------------------------

static constexpr uint32_t kDefaultHostIp   = 0x7F000001u;   // 127.0.0.1
static constexpr uint16_t kDefaultHostPort = 5004;
static constexpr uint16_t kDefaultLocalPort= 5005;

// ---------------------------------------------------------------------------
// Helper: parse "a.b.c.d" → host-byte-order uint32_t
// ---------------------------------------------------------------------------

static uint32_t parseIpv4(const char *s)
{
    struct in_addr a{};
    if (::inet_pton(AF_INET, s, &a) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", s);
        exit(1);
    }
    return ntohl(a.s_addr);  // return host byte order (same as UdpEndpoint.ipv4)
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    uint32_t    hostIp    = kDefaultHostIp;
    uint16_t    hostPort  = kDefaultHostPort;
    uint16_t    localPort = kDefaultLocalPort;
    const char *secret    = nullptr;  // nullptr → no authentication
    bool        discover  = false;    // --discover → browse via mDNS

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--host") == 0 || strcmp(argv[i], "-H") == 0) && i + 1 < argc) {
            hostIp = parseIpv4(argv[++i]);
        } else if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            hostPort = static_cast<uint16_t>(strtoul(argv[++i], nullptr, 10));
        } else if ((strcmp(argv[i], "--local") == 0 || strcmp(argv[i], "-l") == 0) && i + 1 < argc) {
            localPort = static_cast<uint16_t>(strtoul(argv[++i], nullptr, 10));
        } else if ((strcmp(argv[i], "--secret") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc) {
            secret = argv[++i];
        } else if (strcmp(argv[i], "--discover") == 0 || strcmp(argv[i], "-d") == 0) {
            discover = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: nm2_client [--host <ip>] [--port <port>] [--local <port>]\n"
                   "                  [--secret <passphrase>] [--discover]\n");
            printf("  Defaults: host=127.0.0.1 port=%u local=%u, no authentication, no mDNS\n",
                   kDefaultHostPort, kDefaultLocalPort);
            printf("  --discover / -d   : find host via mDNS instead of direct IP\n");
            return 0;
        }
    }

    // -----------------------------------------------------------------------
    // Step 1 — Identify your endpoint (same pattern as main_host.cpp).
    // -----------------------------------------------------------------------

    nm2_example::MidiBridgeApp app("DemoClient", "DEMO-CLIENT-0001");

    networkmidi2::SharedSecretAuthenticator auth(secret ? secret : "");
    if (secret) app.setAuth(&auth);

    networkmidi2::PosixMdnsDiscovery disc;
    if (discover) app.setDiscovery(&disc);

    // -----------------------------------------------------------------------
    // Step 2 — Run in Client role.
    //
    // beginClient() sends the first Invitation immediately.  The session then
    // retransmits every kInviteRetryMs (see Config.h) until:
    //   • ReplyAccepted  → Established
    //   • ReplyPending   → wait, then retry
    //   • ReplyAuth      → begin auth exchange (if IAuthenticator is set)
    //
    // In a product you might discover the host address via mDNS (IDiscovery).
    // Here we connect to a hard-coded or command-line-supplied IP:port.
    // -----------------------------------------------------------------------

    printf("NetworkMIDI2 Client Example\n");
    printf("  Endpoint Name : DemoClient\n");
    printf("  Product ID    : DEMO-CLIENT-0001\n");
    if (!discover) {
        printf("  Host          : %u.%u.%u.%u:%u\n",
               (hostIp >> 24) & 0xFF, (hostIp >> 16) & 0xFF,
               (hostIp >>  8) & 0xFF,  hostIp        & 0xFF,
               hostPort);
    }
    printf("  Local port    : %u\n", localPort);
    printf("  Auth          : %s\n", secret   ? "enabled (--secret)"   : "none");
    printf("  mDNS          : %s\n", discover ? "browsing (--discover)" : "none");
    printf("Press Ctrl-C to close the session.\n\n");

    app.runClient(hostIp, hostPort, localPort);
    return 0;
}
