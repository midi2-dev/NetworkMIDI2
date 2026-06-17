/**
 * @file SessionTask.cpp
 * @brief Network MIDI 2.0 FreeRTOS session task for NXP FRDM-MCXN947.
 *
 * ## Architecture
 * Runs as a single FreeRTOS task (vSessionTask).  lwIP is driven by
 * tcpip_thread independently; all lwIP calls from this task are routed
 * through NxpUdpTransport / NxpMdnsDiscovery which hold LOCK_TCPIP_CORE()
 * for each lwIP raw API call.  No explicit locking in this file.
 *
 * ## Differences from the Pico FreeRTOS example
 *   - No WiFi setup phase — Ethernet link is always on (ENET_QOS).
 *   - No flash-backed config storage (no equivalent to Pico flash API here).
 *   - Debug output via LPUART (printf retargeted by BOARD_InitDebugConsole).
 *   - Input via NM2_GetCharNonBlocking() (NXP SDK DbgConsole_TryGetchar).
 *   - Static IP option removed; DHCP is always used.
 *
 * ## FEC packet-drop simulation (SESSION_RUN keys)
 *   d — drop 1 TX:  piggybacking (kFecDepth=2) recovers with no MIDI gap
 *   D — drop 3 TX:  exceeds FEC depth; receiver sends RetransmitRequest
 *   r — drop 1 RX:  receiving side detects gap and requests retransmit
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

#include "SessionTask.h"
#include "board_init.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "FreeRTOS.h"
#include "task.h"

#include "networkmidi2/NetworkMidiSession.h"
#include "networkmidi2/Types.h"
#include "networkmidi2/SharedSecretAuthenticator.h"
#include "NxpUdpTransport.h"
#include "NxpMdnsDiscovery.h"
#include "DroppingTransport.h"
#include "DemoMidiSource.h"

#include <cstdio>
#include <cstring>
#include <new>

// Route bare printf() → DbgConsole_Printf (NXP LPUART debug console, no buffering).
// Must be included AFTER <cstdio> to avoid polluting std:: namespace declarations.
#include "fsl_debug_console.h"

using namespace networkmidi2;

// ---------------------------------------------------------------------------
// Session and port config
// ---------------------------------------------------------------------------

static constexpr uint16_t    kHostPort   = 5004;
static constexpr uint16_t    kClientPort = 5005;
static constexpr const char *kProductId  = "FRDM-MCXN947-NM2-0001";

// DHCP wait: up to 30 seconds before giving up and prompting for manual IP.
static constexpr uint32_t    kDhcpTimeoutMs = 30000;

// ---------------------------------------------------------------------------
// CLI state machine
// ---------------------------------------------------------------------------

enum class AppState {
    WAIT_DHCP,     // wait for DHCP to assign an IP
    MDNS_NAME,     // enter mDNS base name (e.g. "nxpmidi")
    ROLE_SELECT,   // [H]ost / [C]lient
    AUTH_PASS,     // optional passphrase for SHA-256 auth
    MDNS_RESOLVE,  // client: DNS-SD browse — collecting discovered peers
    MDNS_SELECT,   // client: numbered host selection after browse
    CLIENT_IP,     // client: manual IP entry after mDNS failure / timeout
    SESSION_RUN,   // session live; q/d/D/r keys active
    REPEAT_PROMPT, // [Y]es / [R]ole / [N]o
    DONE
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static NxpUdpTransport            gTransport;
static DroppingTransport          gDropper{gTransport};
static NxpMdnsDiscovery           gDisc;
static nm2_example::DemoMidiSource gDemoSrc;

static NetworkMidiSession *gSession            = nullptr;
static AppState            gState              = AppState::WAIT_DHCP;
static bool                gRunLoop            = true;
static bool                gIsHost             = false;
static bool                gSessionEstablished = false;
static uint32_t            gHostHeartbeatMs    = 0;
static uint32_t            gFecStatsMs         = 0;
static uint32_t            gMdnsRetryMs        = 0;

static char gLineBuf[128] = {};
static int  gLineLen      = 0;
static bool gHideInput    = false;

// Auto-start: if UART RX is not functional, automatically advance prompts
// after kAutoMs milliseconds using built-in defaults.
static constexpr uint32_t kAutoMs      = 60000;   // 60 s
static uint32_t            gPromptMs   = 0;        // time prompt was shown
static bool                gAutoActive = false;    // waiting for auto-advance

static char gMdnsName[32] = {};

static UdpEndpoint gLastHostEp{};

static constexpr int  kMaxFoundPeers = 8;
static DiscoveredPeer gFoundPeers[kMaxFoundPeers];
static int            gFoundCount = 0;

alignas(SharedSecretAuthenticator) static uint8_t gAuthBuf[sizeof(SharedSecretAuthenticator)];
static SharedSecretAuthenticator *gAuth    = nullptr;
static bool                       gHasAuth = false;

// ---------------------------------------------------------------------------
// IP helpers
// ---------------------------------------------------------------------------

static void printIp(uint32_t hostOrderIp)
{
    printf("%u.%u.%u.%u",
           (unsigned)((hostOrderIp >> 24) & 0xFFu),
           (unsigned)((hostOrderIp >> 16) & 0xFFu),
           (unsigned)((hostOrderIp >>  8) & 0xFFu),
           (unsigned)( hostOrderIp        & 0xFFu));
}

// ---------------------------------------------------------------------------
// UMP display
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
        printf("  [UMP] MT4 grp%u %s ch%u note=%u vel=0x%04X\r\n",
               (unsigned)grp, ev, (unsigned)(stat & 0xFu), (unsigned)note, vel);
        return;
    }
    printf("  [UMP] mt=%u words=%zu:", (unsigned)mt, n);
    for (size_t i = 0; i < n && i < 4u; ++i)
        printf(" %08X", (unsigned)w[i]);
    printf("\r\n");
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
    printf("\r\n[nm2] State -> %s\r\n", stateName(s));
    if (s == SessionState::Established) {
        gSessionEstablished = true;
        printf("(type q=quit  d=drop1TX  D=drop3TX  r=drop1RX)\r\n");
    }
    if (s == SessionState::PendingBye && gHasAuth && !gSessionEstablished)
        printf("[AUTH] Session rejected — check passphrase matches on both boards.\r\n");
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
        for (int i = 0; i < 200 && gSession->state() != SessionState::Idle; ++i) {
            gSession->tick();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    delete gSession;
    gSession            = nullptr;
    gDemoSrc            = nm2_example::DemoMidiSource{};
    gSessionEstablished = false;
    gHostHeartbeatMs    = 0;
    gFecStatsMs         = 0;
    gDisc.unadvertise();
    gTransport.close();
}

static void doBeginHost()
{
    destroySession();
    gIsHost = true;

    char epName[64];
    snprintf(epName, sizeof(epName), "%s-host",
             gMdnsName[0] ? gMdnsName : "nxpmidi");

    EndpointInfo info;
    info.setName(epName);
    info.setProductId(kProductId);

    NetworkMidiSession::Callbacks cbs{ onUmp, onStateChange, nullptr };
    gSession = new NetworkMidiSession(gDropper, info, cbs);
    gSession->beginHost(kHostPort, &gDisc, gHasAuth ? gAuth : nullptr);

    printf("\r\n[Host] mDNS  : %s.local\r\n", epName);
    LOCK_TCPIP_CORE();
    ip4_addr_t hostIp = *netif_ip4_addr(netif_default);
    UNLOCK_TCPIP_CORE();
    printf("[Host] IP    : %s  port %u\r\n",
           ip4addr_ntoa(&hostIp), kHostPort);
    if (gHasAuth)
        printf("[Host] Auth  : SHA-256 challenge-response enabled\r\n");
    printf("[Host] Waiting for client...  (q+Enter to quit)\r\n\r\n");

    gHostHeartbeatMs = gTransport.nowMillis();
    gLineLen = 0; gLineBuf[0] = '\0';
    gState = AppState::SESSION_RUN;
}

static void doBeginClient(const UdpEndpoint &hostEp)
{
    destroySession();
    gIsHost     = false;
    gLastHostEp = hostEp;

    char epName[64];
    snprintf(epName, sizeof(epName), "%s-client",
             gMdnsName[0] ? gMdnsName : "nxpmidi");

    EndpointInfo info;
    info.setName(epName);
    info.setProductId(kProductId);

    NetworkMidiSession::Callbacks cbs{ onUmp, onStateChange, nullptr };
    gSession = new NetworkMidiSession(gDropper, info, cbs);
    gSession->beginClient(hostEp, kClientPort, gHasAuth ? gAuth : nullptr);

    printf("[Client] Connecting to ");
    printIp(hostEp.ipv4);
    printf(":%u...\r\n", hostEp.port);
    if (gHasAuth)
        printf("[Client] Auth : SHA-256 challenge-response enabled\r\n");
    printf("(type q=quit  d=drop1TX  D=drop3TX  r=drop1RX)\r\n\r\n");

    gLineLen = 0; gLineBuf[0] = '\0';
    gState = AppState::SESSION_RUN;
}

static void startMdnsResolve()
{
    gFoundCount  = 0;
    printf("\r\nSearching for _midi2._udp hosts via DNS-SD...\r\n"
           "(Enter = stop search and select,  q = quit)\r\n");
    gDisc.browse();
    gMdnsRetryMs = gTransport.nowMillis();
    gState       = AppState::MDNS_RESOLVE;
    gPromptMs    = gTransport.nowMillis();
    gAutoActive  = true;
}

// ---------------------------------------------------------------------------
// handleLine — dispatch the completed input line for the current CLI state
// ---------------------------------------------------------------------------

static void handleLine(const char *line)
{
    const bool blank = (line[0] == '\0');

    switch (gState) {

    case AppState::MDNS_NAME:
        if (!blank) {
            strncpy(gMdnsName, line, sizeof(gMdnsName) - 1);
            gMdnsName[sizeof(gMdnsName) - 1] = '\0';
            for (char *p = gMdnsName; *p; ++p)
                if (*p == ' ') *p = '-';
        } else if (gMdnsName[0] == '\0') {
            strncpy(gMdnsName, "nxpmidi", sizeof(gMdnsName) - 1);
        }
        printf("mDNS name: %s\r\n\r\nRole? [H]ost / [C]lient: ", gMdnsName);
        gState    = AppState::ROLE_SELECT;
        gPromptMs = gTransport.nowMillis();
        gAutoActive = true;
        break;

    case AppState::ROLE_SELECT:
        if (line[0] == 'H' || line[0] == 'h') {
            gIsHost = true;
            printf("Passphrase (Enter to skip authentication): ");
            gHideInput  = true;
            gState      = AppState::AUTH_PASS;
            gPromptMs   = gTransport.nowMillis();
            gAutoActive = true;
        } else if (line[0] == 'C' || line[0] == 'c') {
            gIsHost = false;
            printf("Passphrase (Enter to skip authentication): ");
            gHideInput  = true;
            gState      = AppState::AUTH_PASS;
            gPromptMs   = gTransport.nowMillis();
            gAutoActive = true;
        } else if (!blank) {
            printf("Please enter 'H' or 'C': ");
        }
        break;

    case AppState::AUTH_PASS:
        if (blank) {
            gHasAuth = false;
            gAuth    = nullptr;
            printf("(no authentication)\r\n");
        } else {
            if (gAuth) gAuth->~SharedSecretAuthenticator();
            gAuth    = new (gAuthBuf) SharedSecretAuthenticator(line);
            gHasAuth = true;
            printf("\r\n");
        }
        if (gIsHost)
            doBeginHost();
        else
            startMdnsResolve();
        break;

    case AppState::MDNS_RESOLVE:
        gDisc.stopBrowse();
        if (line[0] == 'q' || line[0] == 'Q') {
            gRunLoop = false;
            gState   = AppState::SESSION_RUN;
        } else if (gFoundCount > 0) {
            printf("\r\nFound %d host(s):\r\n", gFoundCount);
            for (int i = 0; i < gFoundCount; ++i) {
                printf("  [%d] %s  ", i + 1, gFoundPeers[i].epName);
                printIp(gFoundPeers[i].endpoint.ipv4);
                printf(":%u\r\n", gFoundPeers[i].endpoint.port);
            }
            printf("  [0] Enter IP manually\r\nSelect: ");
            gState      = AppState::MDNS_SELECT;
            gPromptMs   = gTransport.nowMillis();
            gAutoActive = true;
        } else {
            printf("Host IP: ");
            gState = AppState::CLIENT_IP;
        }
        break;

    case AppState::MDNS_SELECT: {
        if (line[0] == 'q' || line[0] == 'Q') {
            gRunLoop = false;
            gState   = AppState::SESSION_RUN;
        } else if (line[0] >= '1' && line[0] < '1' + gFoundCount) {
            int idx = line[0] - '1';
            doBeginClient(gFoundPeers[idx].endpoint);
        } else {
            printf("Host IP: ");
            gState = AppState::CLIENT_IP;
        }
        break;
    }

    case AppState::CLIENT_IP: {
        if (blank) { printf("Host IP: "); break; }
        ip4_addr_t addr;
        if (ip4addr_aton(line, &addr) == 0) {
            printf("Invalid address.  Host IP: ");
            break;
        }
        UdpEndpoint ep;
        ep.ipv4 = ntohl(addr.addr);
        ep.port = kHostPort;
        doBeginClient(ep);
        break;
    }

    case AppState::SESSION_RUN:
        if (line[0] == 'q' || line[0] == 'Q') {
            gRunLoop = false;
        } else if (line[0] == 'd') {
            gDropper.scheduleTxDrop(1);
            printf("[FEC] Dropping next 1 TX — piggybacking (kFecDepth=2) recovers seamlessly\r\n");
        } else if (line[0] == 'D') {
            gDropper.scheduleTxDrop(3);
            printf("[FEC] Dropping next 3 TX — exceeds kFecDepth=2; RetransmitRequest in ~100 ms\r\n");
        } else if (line[0] == 'r') {
            gDropper.scheduleRxDrop(1);
            printf("[FEC] Dropping next 1 RX — peer detects gap and sends RetransmitRequest\r\n");
        }
        break;

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
            destroySession();
            printf("\r\nRole? [H]ost / [C]lient: ");
            gState = AppState::ROLE_SELECT;
        } else {
            printf("\r\n[Returning to DHCP wait...]\r\n");
            gState = AppState::WAIT_DHCP;
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
    // Auto-start: if UART RX is unavailable, advance prompts after kAutoMs.
    if (gAutoActive) {
        uint32_t now = gTransport.nowMillis();
        if (now - gPromptMs >= kAutoMs) {
            gAutoActive = false;
            switch (gState) {
            case AppState::MDNS_NAME:
                printf("nxpmidi  [auto]\r\n");
                handleLine("");   // blank → defaults to "nxpmidi"
                break;
            case AppState::ROLE_SELECT:
                printf("H  [auto]\r\n");
                handleLine("H");
                break;
            case AppState::AUTH_PASS:
                printf("  [auto, no auth]\r\n");
                handleLine("");
                break;
            case AppState::MDNS_RESOLVE:
                handleLine("");  // stop browse → show selection list or CLIENT_IP
                break;
            case AppState::MDNS_SELECT:
                if (gFoundCount > 0) {
                    printf("1  [auto]\r\n");
                    handleLine("1");  // auto-select first discovered host
                } else {
                    handleLine("");   // go to CLIENT_IP
                }
                break;
            case AppState::REPEAT_PROMPT:
                printf("Y  [auto]\r\n");
                handleLine("Y");
                break;
            default:
                break;
            }
            return;
        }
    }

    int c = NM2_GetCharNonBlocking();
    if (c < 0) return;

    // Real character received — cancel auto-start countdown.
    gAutoActive = false;
    gPromptMs   = 0;

    if (c == '\r' || c == '\n') {
        gLineBuf[gLineLen] = '\0';
        printf("\r\n");
        handleLine(gLineBuf);
        gLineLen   = 0;
        gHideInput = false;
        return;
    }

    if ((c == 8 || c == 127) && gLineLen > 0) {
        --gLineLen;
        printf("\b \b");
        return;
    }

    if (gLineLen < static_cast<int>(sizeof(gLineBuf)) - 1) {
        gLineBuf[gLineLen++] = static_cast<char>(c);
        if (gHideInput) printf("*");
        else            printf("%c", static_cast<char>(c));
    }
}

// ---------------------------------------------------------------------------
// vSessionTask — FreeRTOS task entry point
// ---------------------------------------------------------------------------

extern "C" void vSessionTask(void * /*params*/)
{
    printf("\r\n=== Network MIDI 2.0 -- NXP FRDM-MCXN947 ===\r\n");
    printf("Product: %s\r\n\r\n", kProductId);
    printf("Waiting for DHCP (up to %u s)...\r\n",
           (unsigned)(kDhcpTimeoutMs / 1000));

    // -----------------------------------------------------------------------
    // Main loop — 1 kHz cadence (vTaskDelay 1 ms)
    // -----------------------------------------------------------------------
    while (gRunLoop) {

        // ---- DHCP wait ----
        if (gState == AppState::WAIT_DHCP) {
            static uint32_t dhcpStartMs  = 0;
            static uint32_t lastHeartbMs = 0;
            if (dhcpStartMs == 0) dhcpStartMs = gTransport.nowMillis();

            LOCK_TCPIP_CORE();
            bool dhcpReady  = netif_is_up(netif_default) &&
                              dhcp_supplied_address(netif_default);
            bool linkUp     = netif_is_link_up(netif_default);
            ip4_addr_t myIp = *netif_ip4_addr(netif_default);
            UNLOCK_TCPIP_CORE();

            if (dhcpReady) {
                printf("IP: %s  (DHCP)\r\n\r\n", ip4addr_ntoa(&myIp));
                dhcpStartMs = 0;
                if (gMdnsName[0])
                    printf("mDNS name (%s — Enter to keep, or type new): ", gMdnsName);
                else
                    printf("mDNS name (e.g. nxpmidi): ");
                gState      = AppState::MDNS_NAME;
                gPromptMs   = gTransport.nowMillis();
                gAutoActive = true;
            } else {
                uint32_t now = gTransport.nowMillis();
                // Print link/DHCP state every 5 s
                if (now - lastHeartbMs >= 5000) {
                    printf("  [link %s, DHCP in progress...]\r\n",
                           linkUp ? "UP" : "DOWN");
                    lastHeartbMs = now;
                }
                if (now - dhcpStartMs >= kDhcpTimeoutMs) {
                    printf("\r\n[warn] DHCP timeout — check Ethernet cable.\r\n"
                           "Retrying...\r\n");
                    dhcpStartMs = now;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (gState == AppState::DONE) { gRunLoop = false; continue; }

        // ---- mDNS browse poll (client) ----
        if (gState == AppState::MDNS_RESOLVE) {
            DiscoveredPeer peer{};
            if (gDisc.nextDiscovered(peer)) {
                // Deduplicate by IP before adding to the display list.
                bool dup = false;
                for (int i = 0; i < gFoundCount; ++i)
                    if (gFoundPeers[i].endpoint.ipv4 == peer.endpoint.ipv4) { dup = true; break; }
                if (!dup && gFoundCount < kMaxFoundPeers) {
                    gFoundPeers[gFoundCount++] = peer;
                    printf("[%d] \"%s\" at ", gFoundCount, peer.epName);
                    printIp(peer.endpoint.ipv4);
                    printf(":%u\r\n", peer.endpoint.port);
                }
            } else {
                uint32_t now = gTransport.nowMillis();
                if (now - gMdnsRetryMs >= 2000) {
                    gDisc.browse();
                    gMdnsRetryMs = now;
                }
            }
        }

        // ---- Session tick + demo MIDI outbound ----
        if (gSession) {
            gSession->tick();
            SessionState s = gSession->state();
            if (s == SessionState::Established) {
                uint32_t w[2];
                if (unsigned wc = gDemoSrc.next(gTransport.nowMillis(), w))
                    gSession->sendUmp(w, wc);
            }

            if (gState == AppState::SESSION_RUN) {
                uint32_t now = gTransport.nowMillis();
                if (gFecStatsMs == 0) gFecStatsMs = now;
                if (now - gFecStatsMs >= 5000) {
                    gFecStatsMs = now;
                    printf("[FEC] TX dropped: %lu  RX dropped: %lu\r\n",
                           (unsigned long)gDropper.txDropped(),
                           (unsigned long)gDropper.rxDropped());
                }
            }

            if (gIsHost && s == SessionState::Idle && !gSessionEstablished &&
                gState == AppState::SESSION_RUN) {
                uint32_t now = gTransport.nowMillis();
                if (now - gHostHeartbeatMs >= 4000) {
                    LOCK_TCPIP_CORE();
                    ip4_addr_t hbIp = *netif_ip4_addr(netif_default);
                    UNLOCK_TCPIP_CORE();
                    printf("[Host] Still waiting at %s:%u  rx=%u  (q+Enter to quit)\r\n",
                           ip4addr_ntoa(&hbIp), kHostPort,
                           (unsigned)gTransport.rxPackets());
                    gHostHeartbeatMs = now;
                }
            }
        }

        // ---- Peer disconnected ----
        if (gState == AppState::SESSION_RUN && gSession &&
            gSessionEstablished && gSession->state() == SessionState::Idle) {
            printf("\r\n[Peer disconnected]\r\n");
            destroySession();
            gRunLoop = false;
        }

        processCli();

        // ---- Transition SESSION_RUN → REPEAT_PROMPT on 'q' or peer-end ----
        if (!gRunLoop && gState == AppState::SESSION_RUN) {
            destroySession();
            gRunLoop = true;
            printf("\r\n[Session closed]\r\n");
            printf("Run again? [Y] same role  [R] new role  [N] restart: ");
            gState      = AppState::REPEAT_PROMPT;
            gPromptMs   = gTransport.nowMillis();
            gAutoActive = true;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // -----------------------------------------------------------------------
    // Final clean-up
    // -----------------------------------------------------------------------
    destroySession();
    printf("\r\n[Session task done — idle]\r\n");
    vTaskSuspend(nullptr);
}
