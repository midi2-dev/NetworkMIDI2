/**
 * @file main_interactive.cpp
 * @brief Network MIDI 2.0 — macOS / Linux interactive CLI example.
 *
 * Mirrors the Pico 2 W lwIP example (examples/midi_bridge/lwip/main.cpp) but
 * runs on any POSIX host.  Useful for testing against a Pico board, a Raspberry
 * Pi, or another desktop over a LAN.
 *
 * ## Features
 *   - No WiFi setup — uses the system's existing network connection.
 *   - Host: advertises via mDNS (_midi2._udp, Bonjour / Avahi).
 *   - Client: auto-discovers via mDNS; press Enter to enter host IP manually.
 *   - Raw-mode terminal for non-blocking input (same UX as the Pico example).
 *   - Ctrl-C triggers a clean Bye / ByeReply shutdown.
 *   - Repeat test without restarting the binary.
 *
 * ## Usage
 *   nm2_interactive [--name <mdns-base-name>] [--port <host-port>]
 *
 *   Both peers must use the same --name (default: picomidi).
 *   The host listens on <host-port> (default 5004); the client uses 5005.
 *
 * ## Example: two terminals on the same machine
 *   Terminal A:  ./build/examples/midi_bridge/nm2_interactive  → role H
 *   Terminal B:  ./build/examples/midi_bridge/nm2_interactive  → role C
 *
 * ## Example: Mac ↔ Raspberry Pi over LAN
 *   Mac:  ./nm2_interactive  → H  (note the IP shown)
 *   Pi:   ./nm2_interactive  → C  (mDNS auto-discovers, or Enter to type IP)
 *
 * ## Example: paired with a Pico 2 W (lwIP example)
 *   Pico: flash nm2_pico.uf2 → connect, role H
 *   Mac:  ./nm2_interactive  → C  (mDNS finds Pico automatically)
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

#include "networkmidi2/NetworkMidiSession.h"
#include "networkmidi2/Types.h"
#include "PosixUdpTransport.h"
#include "PosixMdnsDiscovery.h"
#include "DemoMidiSource.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <termios.h>
#include <unistd.h>

using namespace networkmidi2;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static constexpr uint16_t    kDefaultHostPort = 5004;
static constexpr uint16_t    kClientPort      = 5005;
static constexpr const char *kProductId       = "POSIX-NMIDI-0001";

// ---------------------------------------------------------------------------
// CLI state machine (mirrors the Pico lwIP example, minus WiFi setup)
// ---------------------------------------------------------------------------

enum class AppState {
    MDNS_NAME,      // prompt for mDNS base name (mirrors Pico UX)
    ROLE_SELECT,
    MDNS_RESOLVE,
    CLIENT_IP,
    SESSION_RUN,
    REPEAT_PROMPT,
    DONE
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static PosixUdpTransport             gTransport;
static PosixMdnsDiscovery            gDisc;
static nm2_example::DemoMidiSource   gDemoSrc;

static NetworkMidiSession *gSession           = nullptr;
static AppState            gState             = AppState::MDNS_NAME;
static bool                gRunLoop           = true;
static bool                gIsHost            = false;
static bool                gSessionEstablished = false;
static uint32_t            gHostHeartbeatMs   = 0;

static char     gMdnsName[32] = "picomidi";
static uint16_t gHostPort     = kDefaultHostPort;
static bool     gNameFromArg  = false;   // true when --name was given; skips interactive prompt

static bool        gMdnsSearching = false;   // client mDNS browse in progress
static UdpEndpoint gLastHostEp{};            // repeat same-role on client

// CLI line buffer
static char gLineBuf[128] = {};
static int  gLineLen      = 0;

// Terminal saved state
static struct termios gOrigTermios;
static bool           gRawModeActive = false;

// ---------------------------------------------------------------------------
// Terminal — raw mode lets us poll stdin non-blocking (mirrors Pico CDC input).
// When stdin is a pipe or redirected file (not a TTY), termios is unavailable;
// fall back to O_NONBLOCK on the fd so the session loop still runs freely.
// ---------------------------------------------------------------------------

static bool gNonblockFallback = false;

static void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &gOrigTermios) == 0) {
        struct termios t = gOrigTermios;
        t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        t.c_cc[VMIN]  = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        gRawModeActive = true;
    } else {
        // stdin is a pipe/file — set O_NONBLOCK so read() never blocks the loop
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        gNonblockFallback = true;
    }
}

static void restoreTerminal()
{
    if (gRawModeActive) {
        tcsetattr(STDIN_FILENO, TCSANOW, &gOrigTermios);
        gRawModeActive = false;
    } else if (gNonblockFallback) {
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
        gNonblockFallback = false;
    }
}

// ---------------------------------------------------------------------------
// Signal handler — Ctrl-C triggers a clean Bye / ByeReply shutdown
// ---------------------------------------------------------------------------

static void onSignal(int /*sig*/)
{
    gRunLoop = false;
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------

static void printUmp(const uint32_t *w, size_t n)
{
    if (n == 0) return;
    uint8_t mt = static_cast<uint8_t>((w[0] >> 28) & 0xF);
    if (mt == 4 && n >= 2) {
        uint8_t      grp  = static_cast<uint8_t>((w[0] >> 24) & 0xF);
        uint8_t      stat = static_cast<uint8_t>((w[0] >> 16) & 0xFF);
        uint8_t      note = static_cast<uint8_t>((w[0] >>  8) & 0xFF);
        unsigned int vel  = static_cast<unsigned int>(w[1] >> 16);
        const char  *ev   = (stat & 0xF0u) == 0x90u ? "Note-On " : "Note-Off";
        printf("  [UMP] MT4 grp%u %s ch%u note=%u vel=0x%04X\n",
               static_cast<unsigned>(grp), ev, static_cast<unsigned>(stat & 0xFu),
               static_cast<unsigned>(note), vel);
        fflush(stdout);
        return;
    }
    printf("  [UMP] mt=%u words=%zu:", static_cast<unsigned>(mt), n);
    for (size_t i = 0; i < n && i < 4u; ++i)
        printf(" %08X", static_cast<unsigned>(w[i]));
    printf("\n");
    fflush(stdout);
}

// Print non-loopback IPv4 addresses — gives the host's IP to share with the client peer.
static void printLocalIPs()
{
    struct ifaddrs *ifa;
    if (getifaddrs(&ifa) != 0) return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        auto *sa = reinterpret_cast<struct sockaddr_in *>(i->ifa_addr);
        uint32_t ip = ntohl(sa->sin_addr.s_addr);
        if ((ip >> 24) == 127) continue;   // skip loopback
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
        printf("[Host] IP    : %-16s  (%s)\n", buf, i->ifa_name);
    }
    freeifaddrs(ifa);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Session callbacks
// ---------------------------------------------------------------------------

static const char *stateName(SessionState s)
{
    switch (s) {
    case SessionState::Idle:              return "Idle";
    case SessionState::PendingInvitation: return "PendingInvitation";
    case SessionState::AuthRequired:      return "AuthRequired";
    case SessionState::Established:       return "Established";
    case SessionState::PendingReset:      return "PendingReset";
    case SessionState::PendingBye:        return "PendingBye";
    default:                              return "?";
    }
}

static void onStateChange(void * /*ctx*/, SessionState s)
{
    printf("\n[nm2] State -> %s\n", stateName(s));
    fflush(stdout);
    if (s == SessionState::Established) gSessionEstablished = true;
}

static void onUmp(void * /*ctx*/, const uint32_t *words, size_t count)
{
    printUmp(words, count);
}

// ---------------------------------------------------------------------------
// Session lifecycle helpers
// ---------------------------------------------------------------------------

static void destroySession()
{
    if (!gSession) return;
    if (gSession->state() != SessionState::Idle) {
        gSession->close();
        // Tick until Idle so the Bye / ByeReply exchange completes.
        for (int i = 0; i < 200 && gSession->state() != SessionState::Idle; ++i) {
            gSession->tick();
            usleep(1000);
        }
    }
    delete gSession;
    gSession            = nullptr;
    gDemoSrc            = nm2_example::DemoMidiSource{};
    gSessionEstablished = false;
    gHostHeartbeatMs    = 0;
}

static void doBeginHost()
{
    destroySession();
    gIsHost = true;
    if (gMdnsSearching) { gDisc.stopBrowse(); gMdnsSearching = false; }

    char epName[64];
    snprintf(epName, sizeof(epName), "%s-host", gMdnsName);

    EndpointInfo info;
    info.setName(epName);
    info.setProductId(kProductId);

    NetworkMidiSession::Callbacks cbs{ onUmp, onStateChange, nullptr };
    gSession = new NetworkMidiSession(gTransport, info, cbs);
    // beginHost() calls gDisc.advertise() and will call unadvertise() on session close.
    gSession->beginHost(gHostPort, &gDisc);

    printf("\n[Host] mDNS  : %s.local\n", epName);
    printf("[Host] Port  : %u\n", gHostPort);
    printLocalIPs();
    printf("[Host] Waiting for client...  (q+Enter to quit)\n\n");
    fflush(stdout);
    gHostHeartbeatMs = gTransport.nowMillis();
    gState = AppState::SESSION_RUN;
}

static void doBeginClient(const UdpEndpoint &hostEp)
{
    destroySession();
    gIsHost     = false;
    gLastHostEp = hostEp;
    if (gMdnsSearching) { gDisc.stopBrowse(); gMdnsSearching = false; }

    char epName[64];
    snprintf(epName, sizeof(epName), "%s-client", gMdnsName);

    EndpointInfo info;
    info.setName(epName);
    info.setProductId(kProductId);

    NetworkMidiSession::Callbacks cbs{ onUmp, onStateChange, nullptr };
    gSession = new NetworkMidiSession(gTransport, info, cbs);
    gSession->beginClient(hostEp, kClientPort);

    printf("[Client] Connecting to %u.%u.%u.%u:%u...\n(type 'q' + Enter to quit)\n\n",
           (hostEp.ipv4 >> 24) & 0xFF, (hostEp.ipv4 >> 16) & 0xFF,
           (hostEp.ipv4 >>  8) & 0xFF,  hostEp.ipv4 & 0xFF, hostEp.port);
    fflush(stdout);
    gState = AppState::SESSION_RUN;
}

static void startMdnsResolve()
{
    char hostname[80];
    snprintf(hostname, sizeof(hostname), "%s-host.local", gMdnsName);
    printf("\nSearching for %s via mDNS...\n"
           "(Enter = enter IP manually,  q = quit)\n", hostname);
    fflush(stdout);
    gDisc.browse();
    gMdnsSearching = true;
    gState = AppState::MDNS_RESOLVE;
}

// ---------------------------------------------------------------------------
// handleLine — dispatch completed input line for the current CLI state
// ---------------------------------------------------------------------------

static void handleLine(const char *line)
{
    switch (gState) {

    // ------------------------------------------------------------------
    case AppState::MDNS_NAME:
        if (line[0] != '\0') {
            strncpy(gMdnsName, line, sizeof(gMdnsName) - 1);
            gMdnsName[sizeof(gMdnsName) - 1] = '\0';
            for (char *p = gMdnsName; *p; ++p)
                if (*p == ' ') *p = '-';  // DNS labels must not contain spaces
        }
        printf("mDNS name: %s\n\nRole? [H]ost / [C]lient: ", gMdnsName);
        fflush(stdout);
        gState = AppState::ROLE_SELECT;
        break;

    // ------------------------------------------------------------------
    case AppState::ROLE_SELECT:
        if (line[0] == 'H' || line[0] == 'h') {
            doBeginHost();
        } else if (line[0] == 'C' || line[0] == 'c') {
            startMdnsResolve();
        } else {
            printf("Please enter 'H' or 'C': ");
            fflush(stdout);
        }
        break;

    // ------------------------------------------------------------------
    case AppState::MDNS_RESOLVE:
        if (line[0] == 'q' || line[0] == 'Q') {
            if (gMdnsSearching) { gDisc.stopBrowse(); gMdnsSearching = false; }
            gRunLoop = false;
            gState   = AppState::SESSION_RUN;
        } else {
            // Enter (blank) or any other input: cancel mDNS, prompt for IP
            if (gMdnsSearching) { gDisc.stopBrowse(); gMdnsSearching = false; }
            printf("Host IP: ");
            fflush(stdout);
            gState = AppState::CLIENT_IP;
        }
        break;

    // ------------------------------------------------------------------
    case AppState::CLIENT_IP: {
        if (line[0] == '\0') { printf("Host IP: "); fflush(stdout); break; }
        struct in_addr a{};
        if (inet_pton(AF_INET, line, &a) != 1) {
            printf("Invalid address.  Host IP: ");
            fflush(stdout);
            break;
        }
        UdpEndpoint ep;
        ep.ipv4 = ntohl(a.s_addr);
        ep.port = gHostPort;
        doBeginClient(ep);
        break;
    }

    // ------------------------------------------------------------------
    case AppState::SESSION_RUN:
        if (line[0] == 'q' || line[0] == 'Q')
            gRunLoop = false;
        break;

    // ------------------------------------------------------------------
    case AppState::REPEAT_PROMPT:
        if (line[0] == 'Y' || line[0] == 'y') {
            if (gIsHost) {
                doBeginHost();
            } else if (gLastHostEp.isValid()) {
                doBeginClient(gLastHostEp);
            } else {
                startMdnsResolve();
            }
        } else if (line[0] == 'R' || line[0] == 'r') {
            printf("\nRole? [H]ost / [C]lient: ");
            fflush(stdout);
            gState = AppState::ROLE_SELECT;
        } else {
            // 'N' or anything else — exit
            gRunLoop = false;
            gState   = AppState::DONE;
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// processCli — non-blocking character poll; builds a line, dispatches on Enter
// ---------------------------------------------------------------------------

static void processCli()
{
    char    c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n != 1) return;

    // Ctrl-C arrives as byte 0x03 when ISIG is disabled in raw mode
    if (c == 3) { gRunLoop = false; return; }

    if (c == '\r' || c == '\n') {
        gLineBuf[gLineLen] = '\0';
        printf("\n");
        fflush(stdout);
        handleLine(gLineBuf);
        gLineLen = 0;
        return;
    }

    if ((c == 8 || c == 127) && gLineLen > 0) {
        --gLineLen;
        printf("\b \b");
        fflush(stdout);
        return;
    }

    if (gLineLen < static_cast<int>(sizeof(gLineBuf)) - 1) {
        gLineBuf[gLineLen++] = c;
        printf("%c", c);
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--name") == 0 || strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
            strncpy(gMdnsName, argv[++i], sizeof(gMdnsName) - 1);
            gNameFromArg = true;
        } else if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            gHostPort = static_cast<uint16_t>(strtoul(argv[++i], nullptr, 10));
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: nm2_interactive [--name <mdns-base>] [--port <port>]\n"
                   "  --name / -n   mDNS base name (default: picomidi)\n"
                   "  --port / -p   Host UDP port  (default: %u)\n", kDefaultHostPort);
            return 0;
        }
    }

    // Save terminal state before entering raw mode; atexit restores it on crash too.
    tcgetattr(STDIN_FILENO, &gOrigTermios);
    enableRawMode();
    atexit(restoreTerminal);

    // Keep ISIG disabled; we handle Ctrl-C as byte 0x03 in processCli() so both
    // Ctrl-C and 'q'+Enter produce the same clean-shutdown path.
    signal(SIGTERM, onSignal);

    printf("=== Network MIDI 2.0 -- Interactive (POSIX) ===\n");
    printf("  Host port : %u\n\n", gHostPort);

    if (gNameFromArg) {
        // --name was provided; skip the prompt and go straight to role selection.
        printf("mDNS name: %s\n\nRole? [H]ost / [C]lient: ", gMdnsName);
        gState = AppState::ROLE_SELECT;
    } else {
        printf("mDNS name (%s — Enter to keep, or type new): ", gMdnsName);
        gState = AppState::MDNS_NAME;
    }
    fflush(stdout);

    // -----------------------------------------------------------------------
    // Main loop — 2 kHz cadence matching the Pico lwIP example
    // -----------------------------------------------------------------------
    while (gRunLoop) {

        // Client mDNS browse — poll nextDiscovered() until a host is found.
        // User can press Enter to interrupt and enter the IP manually (useful
        // for cross-VPN sessions where multicast does not reach the peer).
        if (gState == AppState::MDNS_RESOLVE && gMdnsSearching) {
            DiscoveredPeer peer;
            if (gDisc.nextDiscovered(peer)) {
                printf("Found host at %u.%u.%u.%u:%u  (%s)\n",
                       (peer.endpoint.ipv4 >> 24) & 0xFF,
                       (peer.endpoint.ipv4 >> 16) & 0xFF,
                       (peer.endpoint.ipv4 >>  8) & 0xFF,
                        peer.endpoint.ipv4 & 0xFF,
                       peer.endpoint.port, peer.epName);
                fflush(stdout);
                gDisc.stopBrowse();
                gMdnsSearching = false;
                doBeginClient(peer.endpoint);
            }
        }

        // Session tick + demo MIDI outbound
        if (gSession) {
            gSession->tick();
            SessionState s = gSession->state();

            if (s == SessionState::Established) {
                // gSessionEstablished is set in onStateChange callback.
                uint32_t w[2];
                if (unsigned wc = gDemoSrc.next(gTransport.nowMillis(), w))
                    gSession->sendUmp(w, wc);
            }

            // Host prints a periodic "still waiting" heartbeat while idle.
            // SessionState::Idle is the host's normal pre-client state and is
            // NOT an error; the heartbeat just reassures the user it is alive.
            if (gIsHost && s == SessionState::Idle && !gSessionEstablished &&
                gState == AppState::SESSION_RUN) {
                uint32_t now = gTransport.nowMillis();
                if (now - gHostHeartbeatMs >= 4000) {
                    printf("[Host] Listening on port %u ...  (q+Enter to quit)\n", gHostPort);
                    fflush(stdout);
                    gHostHeartbeatMs = now;
                }
            }
        }

        // Peer disconnected — only after a session was previously Established.
        // (Idle before Established is normal for the host role and is ignored here.)
        if (gState == AppState::SESSION_RUN && gSession &&
            gSessionEstablished && gSession->state() == SessionState::Idle) {
            printf("\n[Peer disconnected]\n");
            fflush(stdout);
            destroySession();
            gRunLoop = false;
        }

        // Transition from SESSION_RUN to REPEAT_PROMPT on 'q' / Ctrl-C / peer disconnect
        if (!gRunLoop && gState == AppState::SESSION_RUN) {
            destroySession();
            gRunLoop = true;   // keep alive for repeat prompt
            printf("\n[Session closed]\n");
            printf("Run again? [Y] same role  [R] new role  [N] quit: ");
            fflush(stdout);
            gState = AppState::REPEAT_PROMPT;
        }

        processCli();
        usleep(500);   // 2 kHz — matches the Pico polling cadence
    }

    // -----------------------------------------------------------------------
    // Clean up
    // -----------------------------------------------------------------------
    destroySession();
    if (gMdnsSearching) { gDisc.stopBrowse(); gMdnsSearching = false; }
    gTransport.close();
    restoreTerminal();

    printf("\n[Done]\n");
    return 0;
}
