/**
 * @file main.cpp
 * @brief Network MIDI 2.0 — Pico 2 W interactive example (Phase 8, lwIP polling).
 *
 * ## Features
 *   - WiFi credentials and mDNS name saved to flash (survives power cycle)
 *   - Host role: advertises <name>-host.local via mDNS (_midi2._udp)
 *   - Client role: auto-connects via mDNS, falls back to manual IP entry
 *   - Repeat test without re-entering WiFi credentials
 *   - USB CDC serial CLI (115200, USB; open with screen /dev/cu.usbmodem... 115200)
 *
 * ## First-time flow
 *   1. WiFi SSID
 *   2. WiFi Password
 *   3. mDNS name  (e.g. "picomidi") — enter the SAME name on both boards
 *   4. Role: [H]ost or [C]lient
 *   Host: waits for client, sends and prints MIDI notes
 *   Client: resolves <name>-host.local, connects, sends and prints MIDI notes
 *   5. Type 'q' to end session → offered repeat/reconnect options
 *
 * ## Subsequent boots
 *   Press Enter at the SSID prompt to use saved credentials.
 *   Saved mDNS name appears as default at that prompt too.
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

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"

#include "networkmidi2/NetworkMidiSession.h"
#include "networkmidi2/Types.h"
#include "LwipUdpTransport.h"
#include "LwipMdnsDiscovery.h"
#include "DemoMidiSource.h"

#include <cstdio>
#include <cstring>

using namespace networkmidi2;

// ---------------------------------------------------------------------------
// Flash-backed configuration (last sector of the 4 MB Pico 2 W flash)
// ---------------------------------------------------------------------------

static constexpr uint32_t kFlashMagic  = 0xA4E4D496u; // bumped: adds staticIp/netmask/gateway
static constexpr uint32_t kFlashOffset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;

struct FlashConfig {
    uint32_t magic;
    char     ssid[64];
    char     pass[64];
    char     mdnsName[32];
    char     staticIp[16];  // dotted-decimal; empty string = use DHCP
    char     netmask[16];
    char     gateway[16];
    uint8_t  _pad[FLASH_PAGE_SIZE - 4 - 64 - 64 - 32 - 16 - 16 - 16];
};
static_assert(sizeof(FlashConfig) == FLASH_PAGE_SIZE, "FlashConfig must be one flash page");

static const FlashConfig *kFlashCfg =
    reinterpret_cast<const FlashConfig *>(XIP_BASE + kFlashOffset);

// ---------------------------------------------------------------------------
// Session and network config
// ---------------------------------------------------------------------------

static constexpr uint16_t    kHostPort   = 5004;
static constexpr uint16_t    kClientPort = 5005;
static constexpr const char *kProductId  = "PICO2W-NMIDI-0001";


// ---------------------------------------------------------------------------
// CLI state machine
// ---------------------------------------------------------------------------

enum class AppState {
    WIFI_SSID,      // enter (or confirm saved) SSID
    WIFI_PASS,      // enter password
    WIFI_CONNECTING,// blocking connect (handled in main loop)
    STATIC_IP,      // enter static IP (or Enter for DHCP) — first-time setup only
    MDNS_NAME,      // enter (or confirm saved) mDNS base name
    ROLE_SELECT,    // [H]ost or [C]lient
    MDNS_RESOLVE,   // client: resolving <name>-host.local (handled in main loop)
    CLIENT_IP,      // client: manual IP entry after mDNS failure
    SESSION_RUN,    // session live; 'q' to quit
    REPEAT_PROMPT,  // offer [Y]es / [R]ole / [N]o
    DONE
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static LwipUdpTransport             gTransport;
static LwipMdnsDiscovery            gDisc;
static nm2_example::DemoMidiSource  gDemoSrc;

static NetworkMidiSession  *gSession     = nullptr;
static AppState             gState       = AppState::WIFI_SSID;
static bool                 gRunLoop     = true;
static bool                 gCyw43Inited = false;
static bool                 gIsHost      = false;

// Session lifecycle tracking
static bool     gSessionEstablished = false;  // true once session reaches Established
static uint32_t gHostHeartbeatMs    = 0;      // timestamp of last "waiting" status print

// CLI line buffer
static char  gLineBuf[128] = {};
static int   gLineLen      = 0;
static bool  gHideInput    = false;

// Credentials
static char  gSsid[64]     = {};
static char  gPass[64]     = {};
static char  gMdnsName[32] = {};
static char  gStaticIp[16] = {};  // empty = DHCP
static char  gNetmask[16]  = {};
static char  gGateway[16]  = {};
static bool  gHasSaved     = false;   // true if flash holds valid credentials

// mDNS browse retry timer (client): re-send PTR query every 2 s until found.
static uint32_t   gMdnsRetryMs  = 0;

// Last successfully used host endpoint (for "same role" repeat on client)
static UdpEndpoint gLastHostEp{};

// ---------------------------------------------------------------------------
// Flash helpers
// ---------------------------------------------------------------------------

static void loadConfig()
{
    if (kFlashCfg->magic != kFlashMagic) return;
    memcpy(gSsid,     kFlashCfg->ssid,     sizeof(gSsid));
    memcpy(gPass,     kFlashCfg->pass,     sizeof(gPass));
    memcpy(gMdnsName, kFlashCfg->mdnsName, sizeof(gMdnsName));
    memcpy(gStaticIp, kFlashCfg->staticIp, sizeof(gStaticIp));
    memcpy(gNetmask,  kFlashCfg->netmask,  sizeof(gNetmask));
    memcpy(gGateway,  kFlashCfg->gateway,  sizeof(gGateway));
    gSsid[sizeof(gSsid)-1]         = '\0';  // guarantee null even for max-length flash data
    gPass[sizeof(gPass)-1]         = '\0';
    gMdnsName[sizeof(gMdnsName)-1] = '\0';
    gStaticIp[sizeof(gStaticIp)-1] = '\0';
    gNetmask[sizeof(gNetmask)-1]   = '\0';
    gGateway[sizeof(gGateway)-1]   = '\0';
    gHasSaved = (gSsid[0] != '\0');
}

static void saveConfig()
{
    alignas(4) FlashConfig cfg{};
    cfg.magic = kFlashMagic;
    memcpy(cfg.ssid,     gSsid,     sizeof(cfg.ssid));
    memcpy(cfg.pass,     gPass,     sizeof(cfg.pass));
    memcpy(cfg.mdnsName, gMdnsName, sizeof(cfg.mdnsName));
    memcpy(cfg.staticIp, gStaticIp, sizeof(cfg.staticIp));
    memcpy(cfg.netmask,  gNetmask,  sizeof(cfg.netmask));
    memcpy(cfg.gateway,  gGateway,  sizeof(cfg.gateway));

    // Flash write requires interrupts disabled; safe in polling WiFi mode.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(kFlashOffset, FLASH_SECTOR_SIZE);
    flash_range_program(kFlashOffset, reinterpret_cast<const uint8_t *>(&cfg), sizeof(cfg));
    restore_interrupts(ints);

    gHasSaved = true;
}

// Stop DHCP and set the static address on the default netif.
static void applyStaticIp()
{
    ip4_addr_t ip, nm, gw;
    ip4addr_aton(gStaticIp, &ip);
    ip4addr_aton(gNetmask[0] ? gNetmask : "255.255.255.0", &nm);
    ip4addr_aton(gGateway[0] ? gGateway : "0.0.0.0",       &gw);
    dhcp_stop(netif_default);
    netif_set_addr(netif_default, &ip, &nm, &gw);
    printf("Static IP : %s\r\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
}

// ---------------------------------------------------------------------------
// printUmp — format a received UMP message to stdout
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
               static_cast<unsigned>(grp), ev,
               static_cast<unsigned>(stat & 0xFu),
               static_cast<unsigned>(note), vel);
        return;
    }
    printf("  [UMP] mt=%u words=%zu:", static_cast<unsigned>(mt), n);
    for (size_t i = 0; i < n && i < 4u; ++i)
        printf(" %08X", static_cast<unsigned>(w[i]));
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
    // Set immediately in the callback so gSessionEstablished is true even if
    // the session transitions Established→Idle within the same tick() call.
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
        for (int i = 0; i < 200 && gSession->state() != SessionState::Idle; ++i) {
            if (gCyw43Inited) cyw43_arch_poll();
            gSession->tick();
            sleep_ms(1);
        }
    }
    delete gSession;
    gSession = nullptr;
    gDemoSrc            = nm2_example::DemoMidiSource{};
    gSessionEstablished = false;
    gHostHeartbeatMs    = 0;
    gDisc.unadvertise();   // remove mDNS service so the next role can re-register
    gTransport.close();    // release PCB so it can be reopened on the next role
}

// Helper: format a host-byte-order IPv4 address for display
static void printIp(uint32_t hostOrderIp)
{
    printf("%u.%u.%u.%u",
           (unsigned)((hostOrderIp >> 24) & 0xFFu), (unsigned)((hostOrderIp >> 16) & 0xFFu),
           (unsigned)((hostOrderIp >>  8) & 0xFFu), (unsigned)( hostOrderIp        & 0xFFu));
}

static void doReboot()
{
    printf("\r\n[Rebooting in 0.5 s...]\r\n");
    sleep_ms(500);
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

static void doBeginHost()
{
    destroySession();
    gIsHost = true;

    char epName[64];
    snprintf(epName, sizeof(epName), "%s-host",
             gMdnsName[0] ? gMdnsName : "picomidi");

    EndpointInfo info;
    info.setName(epName);
    info.setProductId(kProductId);

    NetworkMidiSession::Callbacks cbs{ onUmp, onStateChange, nullptr };
    gSession = new NetworkMidiSession(gTransport, info, cbs);
    // beginHost() calls gDisc.advertise(port, info) internally
    gSession->beginHost(kHostPort, &gDisc);

    printf("\r\n[Host] mDNS  : %s.local\r\n", epName);
    printf("[Host] IP    : %s  port %u\r\n",
           ip4addr_ntoa(netif_ip4_addr(netif_default)), kHostPort);
    printf("[Host] Waiting for client...  (q+Enter to quit)\r\n\r\n");
    gHostHeartbeatMs = gTransport.nowMillis();  // first heartbeat 4 s from now
    // Flush any stale characters (e.g. leftover 'q'+Enter from a previous session)
    // so they don't trigger an immediate quit as soon as SESSION_RUN is entered.
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}
    gLineLen   = 0;
    gLineBuf[0] = '\0';
    gState = AppState::SESSION_RUN;
}

static void doBeginClient(const UdpEndpoint &hostEp)
{
    destroySession();
    gIsHost     = false;
    gLastHostEp = hostEp;

    char epName[64];
    snprintf(epName, sizeof(epName), "%s-client",
             gMdnsName[0] ? gMdnsName : "picomidi");

    EndpointInfo info;
    info.setName(epName);
    info.setProductId(kProductId);

    NetworkMidiSession::Callbacks cbs{ onUmp, onStateChange, nullptr };
    gSession = new NetworkMidiSession(gTransport, info, cbs);
    gSession->beginClient(hostEp, kClientPort);

    printf("[Client] Connecting to ");
    printIp(hostEp.ipv4);
    printf(":%u...\r\n(type 'q' + Enter to quit)\r\n\r\n", hostEp.port);
    // Flush stale USB CDC input before entering SESSION_RUN.
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}
    gLineLen   = 0;
    gLineBuf[0] = '\0';
    gState = AppState::SESSION_RUN;
}

static void startMdnsResolve()
{
    printf("\r\nSearching for _midi2._udp hosts via DNS-SD...\r\n"
           "(Enter = enter IP manually,  q = quit)\r\n");
    gDisc.browse();
    gMdnsRetryMs = gTransport.nowMillis();
    gState = AppState::MDNS_RESOLVE;
}

// ---------------------------------------------------------------------------
// handleLine — dispatch the completed line buffer for the current CLI state.
// Called for BOTH empty and non-empty lines (processCli always dispatches).
// ---------------------------------------------------------------------------

static void handleLine(const char *line)
{
    const bool blank = (line[0] == '\0');

    switch (gState) {

    // ------------------------------------------------------------------
    case AppState::WIFI_SSID:
        if (blank) {
            if (gHasSaved) {
                // Use saved credentials — skip to connect
                printf("Using saved WiFi '%s'...\r\n", gSsid);
                gState = AppState::WIFI_CONNECTING;
            } else {
                printf("WiFi SSID: ");   // re-prompt on empty input
            }
        } else {
            strncpy(gSsid, line, sizeof(gSsid) - 1);
            gSsid[sizeof(gSsid) - 1] = '\0';
            printf("WiFi Password: ");
            gHideInput = true;
            gState     = AppState::WIFI_PASS;
        }
        break;

    // ------------------------------------------------------------------
    case AppState::WIFI_PASS:
        strncpy(gPass, line, sizeof(gPass) - 1);
        gPass[sizeof(gPass) - 1] = '\0';
        printf("\r\nConnecting to '%s'...\r\n", gSsid);
        gState = AppState::WIFI_CONNECTING;
        break;

    // ------------------------------------------------------------------
    case AppState::STATIC_IP: {
        if (blank) {
            // DHCP — clear any previously saved static address
            gStaticIp[0] = '\0';
            gNetmask[0]  = '\0';
            gGateway[0]  = '\0';
            printf("Using DHCP.\r\n\r\n");
        } else {
            ip4_addr_t addr;
            if (ip4addr_aton(line, &addr) == 0) {
                printf("Invalid address.  Static IP: ");
                break;
            }
            strncpy(gStaticIp, line, sizeof(gStaticIp) - 1);
            gStaticIp[sizeof(gStaticIp) - 1] = '\0';
            // Derive /24 netmask and .1 gateway — sensible default for home LANs
            uint32_t ip_h = ntohl(addr.addr);
            ip4_addr_t gwAddr;
            ip4_addr_set_u32(&gwAddr, htonl((ip_h & 0xFFFFFF00u) | 1u));
            strncpy(gNetmask, "255.255.255.0",         sizeof(gNetmask) - 1);
            strncpy(gGateway, ip4addr_ntoa(&gwAddr),   sizeof(gGateway) - 1);
            gNetmask[sizeof(gNetmask) - 1] = '\0';
            gGateway[sizeof(gGateway) - 1] = '\0';
            applyStaticIp();
            printf("\r\n");
        }
        saveConfig();
        if (gMdnsName[0])
            printf("mDNS name (%s — Enter to keep, or type new): ", gMdnsName);
        else
            printf("mDNS name (e.g. picomidi): ");
        gState = AppState::MDNS_NAME;
        break;
    }

    // ------------------------------------------------------------------
    case AppState::MDNS_NAME:
        if (!blank) {
            strncpy(gMdnsName, line, sizeof(gMdnsName) - 1);
            gMdnsName[sizeof(gMdnsName) - 1] = '\0';
            // Replace spaces with hyphens (DNS labels must not contain spaces)
            for (char *p = gMdnsName; *p; ++p)
                if (*p == ' ') *p = '-';
        } else if (gMdnsName[0] == '\0') {
            strncpy(gMdnsName, "picomidi", sizeof(gMdnsName) - 1);
        }
        // else: blank + existing saved name → keep it as-is

        saveConfig();
        printf("mDNS name: %s\r\n\r\nRole? [H]ost / [C]lient: ", gMdnsName);
        gState = AppState::ROLE_SELECT;
        break;

    // ------------------------------------------------------------------
    case AppState::ROLE_SELECT:
        if (line[0] == 'H' || line[0] == 'h') {
            doBeginHost();
        } else if (line[0] == 'C' || line[0] == 'c') {
            startMdnsResolve();
        } else if (!blank) {
            printf("Please enter 'H' or 'C': ");
        }
        break;

    // ------------------------------------------------------------------
    case AppState::MDNS_RESOLVE:
        if (line[0] == 'q' || line[0] == 'Q') {
            gDisc.stopBrowse();
            gRunLoop = false;           // triggers the SESSION_RUN quit path
            gState   = AppState::SESSION_RUN;
        } else {
            // Enter (blank) or any other input: cancel browse, fall back to manual IP
            gDisc.stopBrowse();
            printf("Host IP: ");
            gState = AppState::CLIENT_IP;
        }
        break;

    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    case AppState::SESSION_RUN:
        if (line[0] == 'q' || line[0] == 'Q')
            gRunLoop = false;   // handled in main loop (clean shutdown)
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
            destroySession();   // tear down PCB + mDNS before role switch
            printf("\r\nRole? [H]ost / [C]lient: ");
            gState = AppState::ROLE_SELECT;
        } else {
            // 'N' or anything else — reboot so the board is ready for the next run
            doReboot();
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
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) return;

    if (c == '\r' || c == '\n') {
        gLineBuf[gLineLen] = '\0';
        printf("\r\n");
        handleLine(gLineBuf);
        gLineLen   = 0;
        gHideInput = false;
        return;
    }

    // Backspace / DEL
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
// main
// ---------------------------------------------------------------------------

int main()
{
    stdio_init_all();
    sleep_ms(2000);   // wait for USB CDC enumeration

    loadConfig();

    printf("\r\n=== Network MIDI 2.0 -- Pico 2 W ===\r\n");
    printf("Product: %s\r\n\r\n", kProductId);

    if (gHasSaved)
        printf("Saved WiFi: %s\r\n"
               "WiFi SSID (Enter to use saved, or type new): ", gSsid);
    else
        printf("WiFi SSID: ");

    // -----------------------------------------------------------------------
    // Main loop — 2 kHz cadence for WiFi events, session tick, and CLI
    // -----------------------------------------------------------------------
    while (gRunLoop) {

        // ---- Blocking WiFi connect (not interactive) ----
        if (gState == AppState::WIFI_CONNECTING) {
            if (cyw43_arch_init() != 0) {
                printf("[err] CYW43 init failed — reset the board\r\n");
                gState   = AppState::DONE;
                gRunLoop = false;
                continue;
            }
            gCyw43Inited = true;
            cyw43_arch_enable_sta_mode();

            // Retry up to 3 times — the CYW43 driver occasionally returns a
            // transient error (-7 / PICO_ERROR_GENERIC) on first join after a
            // watchdog reboot; a short pause + retry reliably recovers it.
            int r = -1;
            for (int attempt = 1; attempt <= 3 && r != 0; ++attempt) {
                if (attempt > 1) {
                    printf("  Retry %d/3...\r\n", attempt);
                    sleep_ms(1500);
                }
                r = cyw43_arch_wifi_connect_timeout_ms(
                        gSsid, gPass, CYW43_AUTH_WPA2_AES_PSK, 15000);
            }

            if (r != 0) {
                printf("[err] Connect failed after 3 attempts (code %d).\r\n"
                       "Check SSID/password and try again.\r\n"
                       "WiFi SSID (Enter to retry '%s', or type new): ",
                       r, gSsid);
                cyw43_arch_deinit();
                gCyw43Inited = false;
                // Keep gSsid/gPass in memory — user can press Enter to retry
                // with the same credentials without retyping them.
                gState = AppState::WIFI_SSID;
            } else {
                printf("Connected!  IP: %s\r\n",
                       ip4addr_ntoa(netif_ip4_addr(netif_default)));
                bool firstBoot = !gHasSaved;  // capture before saveConfig() sets it true
                saveConfig();   // persist working credentials to flash

                if (!firstBoot && gStaticIp[0]) {
                    // Saved static IP — apply silently, no prompt
                    applyStaticIp();
                    if (gMdnsName[0])
                        printf("mDNS name (%s — Enter to keep, or type new): ", gMdnsName);
                    else
                        printf("mDNS name (e.g. picomidi): ");
                    gState = AppState::MDNS_NAME;
                } else if (!firstBoot) {
                    // Previously chose DHCP — keep using it, no prompt
                    printf("\r\n");
                    if (gMdnsName[0])
                        printf("mDNS name (%s — Enter to keep, or type new): ", gMdnsName);
                    else
                        printf("mDNS name (e.g. picomidi): ");
                    gState = AppState::MDNS_NAME;
                } else {
                    // First-time setup — ask for static IP
                    printf("Static IP  (Enter for DHCP, or e.g. 10.0.0.100): ");
                    gState = AppState::STATIC_IP;
                }
            }
            continue;
        }

        if (gState == AppState::DONE) { gRunLoop = false; continue; }

        // ---- Drive WiFi + lwIP ----
        if (gCyw43Inited) cyw43_arch_poll();

        // ---- DNS-SD browse (client) — poll queue; re-send PTR query every 2 s ----
        if (gState == AppState::MDNS_RESOLVE) {
            DiscoveredPeer peer{};
            if (gDisc.nextDiscovered(peer)) {
                printf("Found host \"%s\" at ", peer.epName);
                printIp(peer.endpoint.ipv4);
                printf(":%u\r\n", peer.endpoint.port);
                gDisc.stopBrowse();
                doBeginClient(peer.endpoint);
            } else {
                // Re-send the PTR query every 2 s until a response arrives or the
                // user presses Enter (manual IP) or q (quit).
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
                // gSessionEstablished is set in onStateChange; just send MIDI here.
                uint32_t w[2];
                if (unsigned wc = gDemoSrc.next(gTransport.nowMillis(), w))
                    gSession->sendUmp(w, wc);
            }

            // Host prints a periodic "still waiting" heartbeat while no client has
            // connected yet.  SessionState::Idle is the host's normal starting state
            // (it listens passively; the client always initiates), so Idle alone is
            // NOT an error — it just means we are waiting for an invitation.
            if (gIsHost && s == SessionState::Idle && !gSessionEstablished &&
                gState == AppState::SESSION_RUN) {
                uint32_t now = gTransport.nowMillis();
                if (now - gHostHeartbeatMs >= 4000) {
                    printf("[Host] Still waiting for client at %s:%u"
                           "  rx=%u  (q+Enter to quit)\r\n",
                           ip4addr_ntoa(netif_ip4_addr(netif_default)), kHostPort,
                           static_cast<unsigned>(gTransport.rxPackets()));
                    gHostHeartbeatMs = now;
                }
            }
        }

        // ---- Session ended by peer (only after a session was Established) ----
        // The host starts and stays in SessionState::Idle while waiting for the
        // client's Invitation — that must NOT be treated as "session ended".
        // Only transition to REPEAT_PROMPT when a previously-Established session
        // drops back to Idle (i.e., the remote side disconnected or timed out).
        if (gState == AppState::SESSION_RUN && gSession &&
            gSessionEstablished && gSession->state() == SessionState::Idle) {
            printf("\r\n[Peer disconnected]\r\n");
            destroySession();
            gRunLoop = false;
        }

        // processCli() runs before the !gRunLoop check so that 'q'+Enter received
        // this tick sets gRunLoop=false in time to be caught below in the same
        // iteration — otherwise the while condition is checked first and the loop
        // exits directly into doReboot() without going through REPEAT_PROMPT.
        processCli();

        // ---- Transition from SESSION_RUN to REPEAT_PROMPT on 'q' or peer-end ----
        if (!gRunLoop && gState == AppState::SESSION_RUN) {
            destroySession();
            gRunLoop = true;   // keep loop alive for repeat prompt
            printf("\r\n[Session closed]\r\n");
            printf("Run again? [Y] same role  [R] new role  [N] reboot: ");
            gState = AppState::REPEAT_PROMPT;
        }

        sleep_us(500);   // ~2 kHz loop; sufficient for 1 ms protocol timers
    }

    // -----------------------------------------------------------------------
    // Final clean-up
    // -----------------------------------------------------------------------
    destroySession();
    gDisc.unadvertise();
    gTransport.close();

    if (gCyw43Inited) {
        cyw43_arch_deinit();
        gCyw43Inited = false;
    }

    doReboot();
}
