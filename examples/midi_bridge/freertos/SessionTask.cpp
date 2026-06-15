/**
 * @file SessionTask.cpp
 * @brief Network MIDI 2.0 FreeRTOS session task — WiFi, mDNS, auth, FEC demo.
 *
 * ## Architecture
 * Runs as a single FreeRTOS task (vSessionTask) using
 * pico_cyw43_arch_lwip_threadsafe_background.  lwIP is driven from an IRQ/alarm
 * context independently of the task; all lwIP raw API calls (tick, mDNS, DHCP)
 * must be wrapped with cyw43_arch_lwip_begin() / cyw43_arch_lwip_end().
 *
 * ## Authentication
 * After role selection the user is prompted for a passphrase.  If non-empty,
 * SharedSecretAuthenticator (SHA-256 challenge-response, M2-124-UM §6.7) is
 * constructed and passed to beginHost()/beginClient().  Both boards must use the
 * same passphrase; a mismatch results in PendingBye + an [AUTH] diagnostic.
 *
 * ## FEC packet-drop simulation (SESSION_RUN keys)
 *   d — drop 1 TX:  piggybacking (kFecDepth=2) recovers with no MIDI gap
 *   D — drop 3 TX:  exceeds FEC depth; receiver sends RetransmitRequest (~100 ms delay)
 *   r — drop 1 RX:  receiving side detects sequence gap and requests retransmit
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
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "FreeRTOS.h"
#include "task.h"

#include "networkmidi2/NetworkMidiSession.h"
#include "networkmidi2/Types.h"
#include "networkmidi2/SharedSecretAuthenticator.h"
#include "LwipUdpTransport.h"
#include "LwipMdnsDiscovery.h"
#include "DroppingTransport.h"
#include "DemoMidiSource.h"
#include "SessionTask.h"

#include <cstdio>
#include <cstring>
#include <new>

using namespace networkmidi2;

// ---------------------------------------------------------------------------
// Flash-backed configuration (last sector of the 4 MB Pico 2 W flash)
// ---------------------------------------------------------------------------

static constexpr uint32_t kFlashMagic  = 0xA4E4D496u;
static constexpr uint32_t kFlashOffset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;

struct FlashConfig {
    uint32_t magic;
    char     ssid[64];
    char     pass[64];
    char     mdnsName[32];
    char     staticIp[16];
    char     netmask[16];
    char     gateway[16];
    uint8_t  _pad[FLASH_PAGE_SIZE - 4 - 64 - 64 - 32 - 16 - 16 - 16];
};
static_assert(sizeof(FlashConfig) == FLASH_PAGE_SIZE, "FlashConfig must be one flash page");

static const FlashConfig *kFlashCfg =
    reinterpret_cast<const FlashConfig *>(XIP_BASE + kFlashOffset);

// ---------------------------------------------------------------------------
// Session and port config
// ---------------------------------------------------------------------------

static constexpr uint16_t    kHostPort   = 5004;
static constexpr uint16_t    kClientPort = 5005;
static constexpr const char *kProductId  = "PICO2W-NMIDI-0001";

// ---------------------------------------------------------------------------
// CLI state machine
// ---------------------------------------------------------------------------

enum class AppState {
    WIFI_SSID,       // enter (or confirm saved) SSID
    WIFI_PASS,       // enter password
    WIFI_CONNECTING, // blocking WiFi connect
    STATIC_IP,       // enter static IP or Enter for DHCP (first boot)
    MDNS_NAME,       // enter (or confirm saved) mDNS base name
    ROLE_SELECT,     // [H]ost / [C]lient
    AUTH_PASS,       // passphrase for SHA-256 auth (Enter to skip)
    MDNS_RESOLVE,    // client: resolving <name>-host.local
    CLIENT_IP,       // client: manual IP entry after mDNS failure
    SESSION_RUN,     // session live; q/d/D/r keys active
    REPEAT_PROMPT,   // [Y]es / [R]ole / [N]o
    DONE
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static LwipUdpTransport             gTransport;
static DroppingTransport            gDropper{gTransport};  // wraps gTransport
static LwipMdnsDiscovery            gDisc;
static nm2_example::DemoMidiSource  gDemoSrc;

static NetworkMidiSession *gSession      = nullptr;
static AppState            gState        = AppState::WIFI_SSID;
static bool                gRunLoop      = true;
static bool                gCyw43Inited  = false;
static bool                gIsHost       = false;

static bool     gSessionEstablished = false;
static uint32_t gHostHeartbeatMs    = 0;
static uint32_t gFecStatsMs         = 0;

// CLI line buffer
static char gLineBuf[128] = {};
static int  gLineLen      = 0;
static bool gHideInput    = false;

// WiFi credentials and mDNS name
static char gSsid[64]     = {};
static char gPass[64]     = {};
static char gMdnsName[32] = {};
static char gStaticIp[16] = {};
static char gNetmask[16]  = {};
static char gGateway[16]  = {};
static bool gHasSaved     = false;

// mDNS browse retry timer
static uint32_t gMdnsRetryMs = 0;

// Reprint initial prompt if terminal connects after banner was first sent
static uint32_t gRepromptMs = 0;

// Last host endpoint used (for same-role repeat on client)
static UdpEndpoint gLastHostEp{};

// Auth — placement new into static buffer so no heap allocation is needed.
// gAuth is null until AUTH_PASS constructs an authenticator.
alignas(SharedSecretAuthenticator) static uint8_t  gAuthBuf[sizeof(SharedSecretAuthenticator)];
static SharedSecretAuthenticator *gAuth    = nullptr;
static bool                       gHasAuth = false;

// ---------------------------------------------------------------------------
// Flash helpers
// ---------------------------------------------------------------------------

static void doEraseProgramFlash(void *arg)
{
    // Runs with Core 1 quiesced (flash_safe_execute guarantee).
    const auto *cfg = static_cast<const FlashConfig *>(arg);
    flash_range_erase(kFlashOffset, FLASH_SECTOR_SIZE);
    flash_range_program(kFlashOffset,
                        reinterpret_cast<const uint8_t *>(cfg), sizeof(*cfg));
}

static void loadConfig()
{
    if (kFlashCfg->magic != kFlashMagic) return;
    memcpy(gSsid,     kFlashCfg->ssid,     sizeof(gSsid));
    memcpy(gPass,     kFlashCfg->pass,     sizeof(gPass));
    memcpy(gMdnsName, kFlashCfg->mdnsName, sizeof(gMdnsName));
    memcpy(gStaticIp, kFlashCfg->staticIp, sizeof(gStaticIp));
    memcpy(gNetmask,  kFlashCfg->netmask,  sizeof(gNetmask));
    memcpy(gGateway,  kFlashCfg->gateway,  sizeof(gGateway));
    gSsid[sizeof(gSsid)-1]         = '\0';
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
    // flash_safe_execute quiesces Core 1 before XIP access — required on RP2350.
    flash_safe_execute(doEraseProgramFlash, &cfg, 500);
    gHasSaved = true;
}

// ---------------------------------------------------------------------------
// IP helpers
// ---------------------------------------------------------------------------

static void applyStaticIp()
{
    ip4_addr_t ip, nm, gw;
    ip4addr_aton(gStaticIp, &ip);
    ip4addr_aton(gNetmask[0] ? gNetmask : "255.255.255.0", &nm);
    ip4addr_aton(gGateway[0] ? gGateway : "0.0.0.0",       &gw);
    cyw43_arch_lwip_begin();
    dhcp_stop(netif_default);
    netif_set_addr(netif_default, &ip, &nm, &gw);
    cyw43_arch_lwip_end();
    printf("Static IP : %s\r\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
}

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
    // PendingBye before Established most likely means auth failure.
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
        cyw43_arch_lwip_begin();
        gSession->close();
        cyw43_arch_lwip_end();
        for (int i = 0; i < 200 && gSession->state() != SessionState::Idle; ++i) {
            cyw43_arch_lwip_begin();
            gSession->tick();
            cyw43_arch_lwip_end();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    delete gSession;
    gSession            = nullptr;
    gDemoSrc            = nm2_example::DemoMidiSource{};
    gSessionEstablished = false;
    gHostHeartbeatMs    = 0;
    gFecStatsMs         = 0;
    cyw43_arch_lwip_begin();
    gDisc.unadvertise();
    gTransport.close();
    cyw43_arch_lwip_end();
}

static void doReboot()
{
    printf("\r\n[Rebooting in 0.5 s...]\r\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    watchdog_reboot(0, 0, 0);
    while (true) taskYIELD();
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
    gSession = new NetworkMidiSession(gDropper, info, cbs);

    cyw43_arch_lwip_begin();
    gSession->beginHost(kHostPort, &gDisc, gHasAuth ? gAuth : nullptr);
    cyw43_arch_lwip_end();

    printf("\r\n[Host] mDNS  : %s.local\r\n", epName);
    printf("[Host] IP    : %s  port %u\r\n",
           ip4addr_ntoa(netif_ip4_addr(netif_default)), kHostPort);
    if (gHasAuth)
        printf("[Host] Auth  : SHA-256 challenge-response enabled\r\n");
    printf("[Host] Waiting for client...  (q+Enter to quit)\r\n\r\n");

    gHostHeartbeatMs = gTransport.nowMillis();
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}
    gLineLen    = 0;
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
    gSession = new NetworkMidiSession(gDropper, info, cbs);

    cyw43_arch_lwip_begin();
    gSession->beginClient(hostEp, kClientPort, gHasAuth ? gAuth : nullptr);
    cyw43_arch_lwip_end();

    printf("[Client] Connecting to ");
    printIp(hostEp.ipv4);
    printf(":%u...\r\n", hostEp.port);
    if (gHasAuth)
        printf("[Client] Auth : SHA-256 challenge-response enabled\r\n");
    printf("(type q=quit  d=drop1TX  D=drop3TX  r=drop1RX)\r\n\r\n");

    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}
    gLineLen    = 0;
    gLineBuf[0] = '\0';
    gState = AppState::SESSION_RUN;
}

static void startMdnsResolve()
{
    printf("\r\nSearching for _midi2._udp hosts via DNS-SD...\r\n"
           "(Enter = enter IP manually,  q = quit)\r\n");
    cyw43_arch_lwip_begin();
    gDisc.browse();
    cyw43_arch_lwip_end();
    gMdnsRetryMs = gTransport.nowMillis();
    gState = AppState::MDNS_RESOLVE;
}

// ---------------------------------------------------------------------------
// handleLine — dispatch the completed input line for the current CLI state
// ---------------------------------------------------------------------------

static void handleLine(const char *line)
{
    const bool blank = (line[0] == '\0');

    switch (gState) {

    case AppState::WIFI_SSID:
        if (blank) {
            if (gHasSaved) {
                printf("Using saved WiFi '%s'...\r\n", gSsid);
                gState = AppState::WIFI_CONNECTING;
            } else {
                printf("WiFi SSID: ");
            }
        } else {
            strncpy(gSsid, line, sizeof(gSsid) - 1);
            gSsid[sizeof(gSsid) - 1] = '\0';
            printf("WiFi Password: ");
            gHideInput = true;
            gState     = AppState::WIFI_PASS;
        }
        break;

    case AppState::WIFI_PASS:
        strncpy(gPass, line, sizeof(gPass) - 1);
        gPass[sizeof(gPass) - 1] = '\0';
        printf("\r\nConnecting to '%s'...\r\n", gSsid);
        gState = AppState::WIFI_CONNECTING;
        break;

    case AppState::STATIC_IP: {
        if (blank) {
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
            uint32_t ip_h = ntohl(addr.addr);
            ip4_addr_t gwAddr;
            ip4_addr_set_u32(&gwAddr, htonl((ip_h & 0xFFFFFF00u) | 1u));
            strncpy(gNetmask, "255.255.255.0",       sizeof(gNetmask) - 1);
            strncpy(gGateway, ip4addr_ntoa(&gwAddr),  sizeof(gGateway) - 1);
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

    case AppState::MDNS_NAME:
        if (!blank) {
            strncpy(gMdnsName, line, sizeof(gMdnsName) - 1);
            gMdnsName[sizeof(gMdnsName) - 1] = '\0';
            for (char *p = gMdnsName; *p; ++p)
                if (*p == ' ') *p = '-';
        } else if (gMdnsName[0] == '\0') {
            strncpy(gMdnsName, "picomidi", sizeof(gMdnsName) - 1);
        }
        saveConfig();
        printf("mDNS name: %s\r\n\r\nRole? [H]ost / [C]lient: ", gMdnsName);
        gState = AppState::ROLE_SELECT;
        break;

    case AppState::ROLE_SELECT:
        if (line[0] == 'H' || line[0] == 'h') {
            gIsHost = true;
            printf("Passphrase (Enter to skip authentication): ");
            gHideInput = true;
            gState     = AppState::AUTH_PASS;
        } else if (line[0] == 'C' || line[0] == 'c') {
            gIsHost = false;
            printf("Passphrase (Enter to skip authentication): ");
            gHideInput = true;
            gState     = AppState::AUTH_PASS;
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
            // Placement new into static buffer — no heap allocation.
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
        if (line[0] == 'q' || line[0] == 'Q') {
            cyw43_arch_lwip_begin();
            gDisc.stopBrowse();
            cyw43_arch_lwip_end();
            gRunLoop = false;
            gState   = AppState::SESSION_RUN;
        } else {
            cyw43_arch_lwip_begin();
            gDisc.stopBrowse();
            cyw43_arch_lwip_end();
            printf("Host IP: ");
            gState = AppState::CLIENT_IP;
        }
        break;

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
    // USB CDC was enumerated during sleep_ms(2000) in main() before the
    // scheduler started.  Give the host a moment to open the port.
    vTaskDelay(pdMS_TO_TICKS(500));

    loadConfig();

    printf("\r\n=== Network MIDI 2.0 -- Pico 2 W [FreeRTOS] ===\r\n");
    printf("Product: %s\r\n\r\n", kProductId);

    if (gHasSaved)
        printf("Saved WiFi: %s\r\n"
               "WiFi SSID (Enter to use saved, or type new): ", gSsid);
    else
        printf("WiFi SSID: ");

    // -----------------------------------------------------------------------
    // Main loop — 1 kHz cadence (vTaskDelay 1 ms) for session tick and CLI
    // -----------------------------------------------------------------------
    while (gRunLoop) {

        // ---- Blocking WiFi connect ----
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
            // transient error on first join after a watchdog reboot.
            int r = -1;
            for (int attempt = 1; attempt <= 3 && r != 0; ++attempt) {
                if (attempt > 1) {
                    printf("  Retry %d/3...\r\n", attempt);
                    vTaskDelay(pdMS_TO_TICKS(1500));
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
                gState = AppState::WIFI_SSID;
            } else {
                printf("Connected!  IP: %s\r\n",
                       ip4addr_ntoa(netif_ip4_addr(netif_default)));
                bool firstBoot = !gHasSaved;
                saveConfig();
                if (!firstBoot && gStaticIp[0]) {
                    applyStaticIp();
                    if (gMdnsName[0])
                        printf("mDNS name (%s — Enter to keep, or type new): ", gMdnsName);
                    else
                        printf("mDNS name (e.g. picomidi): ");
                    gState = AppState::MDNS_NAME;
                } else if (!firstBoot) {
                    printf("\r\n");
                    if (gMdnsName[0])
                        printf("mDNS name (%s — Enter to keep, or type new): ", gMdnsName);
                    else
                        printf("mDNS name (e.g. picomidi): ");
                    gState = AppState::MDNS_NAME;
                } else {
                    printf("Static IP  (Enter for DHCP, or e.g. 10.0.0.100): ");
                    gState = AppState::STATIC_IP;
                }
            }
            continue;
        }

        if (gState == AppState::DONE) { gRunLoop = false; continue; }

        // ---- mDNS browse poll (client) ----
        if (gState == AppState::MDNS_RESOLVE) {
            DiscoveredPeer peer{};
            if (gDisc.nextDiscovered(peer)) {
                printf("Found host \"%s\" at ", peer.epName);
                printIp(peer.endpoint.ipv4);
                printf(":%u\r\n", peer.endpoint.port);
                cyw43_arch_lwip_begin();
                gDisc.stopBrowse();
                cyw43_arch_lwip_end();
                doBeginClient(peer.endpoint);
            } else {
                // Re-send PTR query every 2 s until a response arrives.
                uint32_t now = gTransport.nowMillis();
                if (now - gMdnsRetryMs >= 2000) {
                    cyw43_arch_lwip_begin();
                    gDisc.browse();
                    cyw43_arch_lwip_end();
                    gMdnsRetryMs = now;
                }
            }
        }

        // ---- Session tick + demo MIDI outbound ----
        if (gSession) {
            SessionState s;
            cyw43_arch_lwip_begin();
            gSession->tick();
            s = gSession->state();
            if (s == SessionState::Established) {
                uint32_t w[2];
                if (unsigned wc = gDemoSrc.next(gTransport.nowMillis(), w))
                    gSession->sendUmp(w, wc);
            }
            cyw43_arch_lwip_end();

            // Periodic FEC drop stats
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

            // Host prints a "still waiting" heartbeat while idle before first connect.
            if (gIsHost && s == SessionState::Idle && !gSessionEstablished &&
                gState == AppState::SESSION_RUN) {
                uint32_t now = gTransport.nowMillis();
                if (now - gHostHeartbeatMs >= 4000) {
                    printf("[Host] Still waiting for client at %s:%u"
                           "  rx=%u  (q+Enter to quit)\r\n",
                           ip4addr_ntoa(netif_ip4_addr(netif_default)), kHostPort,
                           (unsigned)gTransport.rxPackets());
                    gHostHeartbeatMs = now;
                }
            }
        }

        // ---- Peer disconnected (Established → Idle) ----
        if (gState == AppState::SESSION_RUN && gSession &&
            gSessionEstablished && gSession->state() == SessionState::Idle) {
            printf("\r\n[Peer disconnected]\r\n");
            destroySession();
            gRunLoop = false;
        }

        // processCli() before the !gRunLoop check so 'q'+Enter is caught
        // and sets gRunLoop=false within the same iteration.
        processCli();

        // ---- Re-print initial prompt every 3 s while waiting for SSID input ----
        // Handles the common case where the terminal connects after the first
        // banner print (CDC enumerated before the host app opened the port).
        if (gState == AppState::WIFI_SSID && gLineLen == 0) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (gRepromptMs == 0) gRepromptMs = now;
            if (now - gRepromptMs >= 3000) {
                gRepromptMs = now;
                if (gHasSaved)
                    printf("\r\nSaved WiFi: %s\r\n"
                           "WiFi SSID (Enter to use saved, or type new): ", gSsid);
                else
                    printf("\r\nWiFi SSID: ");
            }
        } else {
            gRepromptMs = 0;
        }

        // ---- Transition SESSION_RUN → REPEAT_PROMPT on 'q' or peer-end ----
        if (!gRunLoop && gState == AppState::SESSION_RUN) {
            destroySession();
            gRunLoop = true;
            printf("\r\n[Session closed]\r\n");
            printf("Run again? [Y] same role  [R] new role  [N] reboot: ");
            gState = AppState::REPEAT_PROMPT;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // -----------------------------------------------------------------------
    // Final clean-up
    // -----------------------------------------------------------------------
    destroySession();
    if (gCyw43Inited) {
        cyw43_arch_deinit();
        gCyw43Inited = false;
    }
    doReboot();
}
