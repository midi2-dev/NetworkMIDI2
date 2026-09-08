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
 * ## Console UX
 * Deliberately unified with the Pico/ProtoZOA DEVICE-role bridge's boot-time
 * setup menu and run loop (examples/midi_bridge/pico/console_menu.cpp and
 * main.cpp): same "Current configuration" printout, same 3-second "press any
 * key to enter setup (or press ESC any time later)" gate, same Role -> Name
 * prompt order and wording, same client host-discovery wording/timing, same
 * "Bridge running. Press ESC at any time to re-enter setup." message, same
 * in-place ESC-reenter (no MCU reset -- see kReconfigureKey below), and no
 * other keys handled once a session is running -- Pico's run loop checks
 * only ESC. This file used to also offer SHA-256 auth, FEC packet-drop test
 * keys, a 'q' quit key, and a Y/R/N repeat-prompt flow; all removed, at the
 * user's explicit request, so the two boards' UX is identical rather than
 * NXP carrying bench-only extras Pico doesn't have.
 *
 * Kept as a separate, non-blocking async state machine rather than sharing
 * source with console_menu.cpp: Pico's menu blocks synchronously inside its
 * own wait loops (pumping tud_task() itself while parked in readLine()),
 * which only works because that firmware has nothing else to do
 * concurrently. This task shares the MCU with FreeRTOS's independent USB and
 * lwIP/tcpip_thread tasks and must never block for more than ~1 tick, so it
 * is structured as a state machine polled once per loop iteration instead.
 * Keep the two files' *wording* in sync by hand when either changes; this
 * comment is the reminder.
 *
 * ## Differences from the Pico DEVICE-role example that remain, deliberately
 *   - No WiFi setup phase — Ethernet link is always on (ENET_QOS).
 *   - No flash-backed config storage -- role/name/host selection are RAM-only
 *     and reset to defaults on every power cycle (no equivalent to Pico's
 *     flash API exists for this board in this repo yet). Made visible to the
 *     user via a note in the "Current configuration" printout rather than
 *     silently diverging from Pico's persisted behavior.
 *   - Debug output via LPUART (printf retargeted by BOARD_InitDebugConsole).
 *   - Input via NM2_GetCharNonBlocking() (NXP SDK DbgConsole_TryGetchar).
 *   - A 60-second failsafe auto-advances any setup prompt with a sensible
 *     default if nobody answers (kAutoMs) -- unlike Pico, this task cannot
 *     block indefinitely in a prompt without starving FreeRTOS's other
 *     tasks' fair share forever on an unattended boot with no working
 *     console. This is invisible to someone actually at the console (it
 *     only fires after a full minute of silence) and isn't part of the UX
 *     Pico and NXP are meant to share -- it exists only because this task
 *     cannot use Pico's "just block in readLine()" approach at all.
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
#include "NxpUdpTransport.h"
#include "NxpMdnsDiscovery.h"
#if NM2_BRIDGE_USB_HOST
#include "host/usbh.h"   // tuh_vid_pid_get()
#include "ump_host.h"
#else
#include "ump_device.h"
#include "include/umpProcessor.h"
#include "include/umpMessageCreate.h"
#endif

#include <cstdio>
#include <cstring>

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

// Boot-time "press any key to enter setup" gate -- same 3 s window and
// wording as Pico's console_menu.cpp (kSetupPromptTimeoutMs there).
static constexpr uint32_t    kSetupPromptTimeoutMs = 3000;

// Client host-discovery browse window -- same 4 s window as Pico's
// console_menu.cpp (kHostBrowseMs there).
static constexpr uint32_t    kHostBrowseMs = 4000;

// Pressing this key at any time while a session is running re-enters setup
// in place -- no MCU reset. Matches examples/midi_bridge/pico/main.cpp's
// kReconfigureKey exactly (same key, same "why", see its comment there):
// unlike Pico, this app never had a reboot-based reconfigure path to begin
// with (doBeginHost()/doBeginClient() already construct a fresh
// NetworkMidiSession each time, so there's no non-movable-session
// constraint to work around here) -- this is purely about matching the UX.
static constexpr int kReconfigureKey = 0x1B; // ESC

// ---------------------------------------------------------------------------
// CLI state machine
// ---------------------------------------------------------------------------

enum class AppState {
    BOOT_PROMPT,     // "press any key within 3s to enter setup..." gate
    ROLE_SELECT,     // [C]lient or [H]ost -- first setup question, matching Pico
    MDNS_NAME,       // "Network MIDI name" -- second, matching Pico
    NETWORK_SELECT,  // [D]HCP/link-local or [S]tatic IP -- third, matching Pico
    STATIC_IP,       // static-IP sub-prompt chain (only if Static chosen)
    STATIC_NETMASK,
    STATIC_GATEWAY,
    STATIC_DNS,
    WAIT_NETWORK,    // bring the netif up per the decided config; wait for it to be ready
    MDNS_RESOLVE,    // client: DNS-SD browse — collecting discovered peers
    CLIENT_IP,       // client: manual IP entry after mDNS failure / 'm'
    SESSION_RUN,     // session live (or idle with no host yet); ESC active
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static NxpUdpTransport            gTransport;
static NxpMdnsDiscovery           gDisc;

#if NM2_BRIDGE_USB_HOST
// ---------------------------------------------------------------------------
// HOST role: track the single currently-mounted USB MIDI device. This
// bridge assumes one downstream USB MIDI device at a time (matching the
// DEVICE role's implicit single-device semantics) -- ump_host.cpp itself
// supports several simultaneously (see UUT/USB_Host_UMP_Test's
// MAX_MOUNTED_UMP table) if this ever needs to extend to more than one.
// Same pattern as examples/midi_bridge/pico/main.cpp's HOST role.
// ---------------------------------------------------------------------------
// String descriptor scratch buffer + UTF-16LE decode helper, for printing
// the attached device's manufacturer/product/serial-number strings in
// tuh_ump_mount_cb() below. Adapted from TinyUSB's own reference example
// (third_party/tinyusb/examples/host/device_info/src/main.c) -- that
// example proves the *_sync() descriptor calls used here are safe even
// though (per tuh_control_xfer()'s own comment in usbh.c) they run a
// nested/reentrant tuh_task() pump while blocking: TinyUSB's task loop is
// deliberately written to tolerate that, so this is a supported pattern,
// not the deadlock risk it would be for most task-loop designs.
#define NM2_USB_LANGUAGE_ID_EN 0x0409
CFG_TUH_MEM_SECTION static struct {
    TUH_EPBUF_DEF(buf, 128 * sizeof(uint16_t));
} s_usbStr;

static void nm2PrintUsbUtf16(uint16_t *buf16, size_t buf16Len)
{
    if ((buf16[0] & 0xFF) == 0) { printf("(none)"); return; }
    size_t len = ((buf16[0] & 0xFF) - 2) / sizeof(uint16_t);
    if (len > buf16Len - 1) len = buf16Len - 1;
    for (size_t i = 0; i < len; ++i) {
        uint16_t c = buf16[1 + i];
        putchar(c < 0x80 ? (char) c : '?'); // non-ASCII: printable placeholder
    }
}

static bool    s_usbHostMounted = false;
static uint8_t s_usbHostDaddr   = 0;
static uint8_t s_usbHostItfNum  = 0;

// Invoked when a UMP interface finishes enumeration (ump_host.cpp). Prints
// enough about the attached device to diagnose "enumerates but no MIDI
// data flows" reports without needing a debugger: manufacturer/product/
// serial-number strings, VID/PID, which alt setting the driver actually
// landed on (0 = legacy MIDI 1.0 byte stream, 1 = native UMP -- a
// MIDI-1-only device staying on alt 0 changes how bytes are framed and is
// a likely first suspect if raw pass-through looks like nothing is
// happening), the MIDIStreaming class-spec version (bcdMSC), and the
// Group Terminal Block table ump_host.cpp parsed (or synthesized, for an
// alt-0-only device) for it.
//
// The string-descriptor fetches (tuh_descriptor_get_*_string_sync) do a
// real blocking control transfer -- safe to call from here despite this
// callback running from inside tuh_task()'s own call stack (via
// umph_finish_mount() -> usbh_driver_set_config_complete()): per
// tuh_control_xfer()'s own comment in usbh.c, the sync wait loop
// deliberately re-invokes tuh_task() reentrantly while blocked, and
// TinyUSB's own reference example (third_party/tinyusb/examples/host/
// device_info) calls these same sync APIs from tuh_mount_cb(), which
// fires from the same nesting depth -- a supported pattern, not a
// deadlock risk.
void tuh_ump_mount_cb(uint8_t daddr, uint8_t itf_num) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);

    uint8_t  altSetting = tuh_ump_alt_setting(daddr, itf_num);
    uint16_t bcdMsc     = tuh_ump_get_bcd_msc(daddr, itf_num);

    printf("[%lu] USB HOST: UMP device mounted daddr=%u itf_num=%u\r\n",
           (unsigned long)gTransport.nowMillis(), daddr, itf_num);

    printf("  Manufacturer: ");
    if (tuh_descriptor_get_manufacturer_string_sync(daddr, NM2_USB_LANGUAGE_ID_EN,
                                                     s_usbStr.buf, sizeof(s_usbStr.buf)) == XFER_RESULT_SUCCESS) {
        nm2PrintUsbUtf16((uint16_t *) (uintptr_t) s_usbStr.buf, sizeof(s_usbStr.buf) / 2);
    } else {
        printf("(none)");
    }
    printf("\r\n  Product:      ");
    if (tuh_descriptor_get_product_string_sync(daddr, NM2_USB_LANGUAGE_ID_EN,
                                                s_usbStr.buf, sizeof(s_usbStr.buf)) == XFER_RESULT_SUCCESS) {
        nm2PrintUsbUtf16((uint16_t *) (uintptr_t) s_usbStr.buf, sizeof(s_usbStr.buf) / 2);
    } else {
        printf("(none)");
    }
    printf("\r\n  Serial:       ");
    if (tuh_descriptor_get_serial_string_sync(daddr, NM2_USB_LANGUAGE_ID_EN,
                                               s_usbStr.buf, sizeof(s_usbStr.buf)) == XFER_RESULT_SUCCESS) {
        nm2PrintUsbUtf16((uint16_t *) (uintptr_t) s_usbStr.buf, sizeof(s_usbStr.buf) / 2);
    } else {
        printf("(none)");
    }
    printf("\r\n  VID=0x%04X PID=0x%04X alt=%u (%s) bcdMSC=0x%04X\r\n",
           (unsigned)vid, (unsigned)pid, (unsigned)altSetting,
           altSetting == 1 ? "native UMP" : "legacy MIDI 1.0 byte stream",
           (unsigned)bcdMsc);

    midi2_desc_group_terminal_block_t const *gtb = nullptr;
    uint8_t gtbCount = tuh_ump_get_group_terminal_blocks(daddr, itf_num, &gtb);
    printf("  Group Terminal Blocks: %u\r\n", (unsigned)gtbCount);
    for (uint8_t i = 0; i < gtbCount && gtb; ++i) {
        static const char *kDirName[] = {"bidirectional", "IN only", "OUT only", "?"};
        printf("    [%u] ID=%u type=%s groups=%u-%u protocol=0x%02X\r\n",
               (unsigned)i, (unsigned)gtb[i].bGrpTrmBlkID,
               kDirName[gtb[i].bGrpTrmBlkType & 0x3],
               (unsigned)gtb[i].nGroupTrm,
               (unsigned)(gtb[i].nGroupTrm + (gtb[i].nNumGroupTrm ? gtb[i].nNumGroupTrm - 1 : 0)),
               (unsigned)gtb[i].bMIDIProtocol);
    }

    s_usbHostMounted = true;
    s_usbHostDaddr   = daddr;
    s_usbHostItfNum  = itf_num;
}

void tuh_ump_umount_cb(uint8_t daddr, uint8_t itf_num) {
    printf("[%lu] USB HOST: UMP device unmounted daddr=%u itf_num=%u\r\n",
           (unsigned long)gTransport.nowMillis(), daddr, itf_num);
    if (s_usbHostMounted && s_usbHostDaddr == daddr && s_usbHostItfNum == itf_num) {
        s_usbHostMounted = false;
    }
}

#else // DEVICE role

// tud_ump_* API "itf" parameter -- NOT the USB descriptor interface number
// (which is 1, see usb_descriptors.cpp's single MIDIStreaming interface,
// alt-setting 1 = UMP mode). It's the 0-based slot index into
// third_party/tusb_ump/ump_device.cpp's internal _umpd_itf[CFG_TUD_UMP]
// array (tud_ump_n_mounted()/_write_hton()/_read_ntoh() etc. all do
// `_umpd_itf[itf]` directly) -- always 0 here since CFG_TUD_UMP=1 (a single
// UMP function). A real bug lived here for a while: this used to be set to
// 1 (confusing it with the USB interface number above), which silently
// targeted an unopened, uninitialized array slot -- every tud_ump_write_hton
// call appeared to succeed (no error return checked) but the data never
// reached the real endpoint, and reads always saw nothing available, so
// MIDI data never flowed in either direction despite the device enumerating
// and CoreMIDI naming its ports correctly (that naming comes from the
// static Group Terminal Block descriptor via control transfers, keyed by
// the real interface number in usb_descriptors.cpp's epInterface[]={1} --
// unrelated to this constant, which is why enumeration/naming looked fine
// while data transfer was completely broken). Matches Pico's DEVICE-role
// build (examples/midi_bridge/pico/main.cpp), which hardcodes 0 for the
// same reason.
static constexpr uint8_t kUmpItf = 0;

// Minimal UMP Endpoint Discovery identity, matching the Pico DEVICE-role
// build (examples/midi_bridge/pico/main.cpp) and DIN_Bridge -- AmeNote does
// not have a registered SysEx manufacturer ID, so this uses the reserved
// "educational/non-commercial" prefix (0x7D) per the MIDI Association spec.
#define DEVICE_MFRID 0x7D, 0x00, 0x00
#define DEVICE_FAMID 0x00, 0x00
#define DEVICE_MODELID 0x00, 0x00
#define DEVICE_VERSIONID 0, 1, 0, 0

static umpProcessor UMPHandler;

static void midiendpoint(uint8_t majVer, uint8_t minVer, uint8_t filter);
static void functionblock(uint8_t fbIdx, uint8_t filter);
#endif // NM2_BRIDGE_USB_HOST

static NetworkMidiSession *gSession            = nullptr;
static AppState            gState              = AppState::BOOT_PROMPT;
static bool                gIsHost             = false;
static bool                gSessionEstablished = false;
static uint32_t            gHostHeartbeatMs    = 0;

// Network mode -- RAM-only, same "(not persisted...)" caveat as role/name
// (see file header comment). Defaults match Pico's bridge_config.h.
static bool gUseDhcp          = true;
static char gStaticIp[16]     = "192.168.1.200";
static char gStaticNetmask[16] = "255.255.255.0";
static char gStaticGateway[16] = "192.168.1.1";
static char gStaticDns[16]     = "192.168.1.1";

// True once the interactive setup walk (Role -> Name -> Network[+static])
// has been completed at least once this boot -- used the same way as
// Pico's main.cpp `enteredSetup`: it's what decides whether a Client role
// goes straight into mDNS host discovery once the network comes up, vs.
// staying idle waiting for ESC (see proceedAfterNetworkReady() below).
static bool gEnteredSetup = false;
static uint32_t            gMdnsRetryMs        = 0;

static char gLineBuf[128] = {};
static int  gLineLen      = 0;

// Auto-start: if UART RX is not functional, automatically advance prompts
// after kAutoMs milliseconds using built-in defaults. See the file header
// comment -- this has no Pico equivalent and is invisible to anyone actually
// at the console.
static constexpr uint32_t kAutoMs      = 60000;   // 60 s
static uint32_t            gPromptMs   = 0;        // time prompt was shown
static bool                gAutoActive = false;    // waiting for auto-advance

static char     gMdnsName[32]     = {};
static uint32_t gBootDeadlineMs   = 0; // BOOT_PROMPT's own 3 s countdown

static UdpEndpoint gLastHostEp{};

static constexpr int  kMaxFoundPeers = 8;
static DiscoveredPeer gFoundPeers[kMaxFoundPeers];
static int            gFoundCount = 0;
static uint32_t       gBrowseDeadlineMs = 0; // MDNS_RESOLVE's 4 s browse window

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

// The mDNS base name as it will actually be used -- gMdnsName is empty on a
// fresh boot (RAM-only config, see the file header comment), so every place
// that needs "the name" falls back to the same default. Centralised here so
// the "Current configuration" printout and the actual session name can never
// drift apart.
static const char *effectiveName()
{
    return gMdnsName[0] ? gMdnsName : "nxpmidi";
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

// Same wording as Pico's onNetworkStateChange() in main.cpp
// ("[<timestamp>] NM2 session state -> %s"). Pico's timestamp is
// time_us_64() (microseconds); this board has no equivalent free-running
// microsecond timer wired up here, so this uses gTransport.nowMillis()
// instead -- same structure, coarser unit, still just a debug ordering aid.
static void onStateChange(void * /*ctx*/, SessionState s)
{
    printf("[%lu] NM2 session state -> %s\r\n",
           (unsigned long)gTransport.nowMillis(), stateName(s));
    if (s == SessionState::Established) {
        gSessionEstablished = true;
    }
}

#if NM2_BRIDGE_USB_HOST
static void onUmp(void * /*ctx*/, const uint32_t *words, size_t count)
{
    printUmp(words, count);
    // Forward network-received UMP out to the attached USB MIDI device, if
    // one is mounted. No local Endpoint/Function Block Discovery here --
    // ump_host.cpp doesn't implement the UMP Stream-message handshake yet
    // (see its header's scope notes), so this is a raw pass-through, same
    // as examples/midi_bridge/pico/main.cpp's HOST role.
    if (!s_usbHostMounted) return;
    tuh_ump_write_hton(s_usbHostDaddr, s_usbHostItfNum,
                       const_cast<uint32_t *>(words), static_cast<uint16_t>(count));
}
#else
static void onUmp(void * /*ctx*/, const uint32_t *words, size_t count)
{
    printUmp(words, count);
    // Forward network-received UMP out the USB MIDI 2.0 device interface.
    tud_ump_write_hton(kUmpItf, const_cast<uint32_t *>(words), static_cast<uint16_t>(count));
}

// ---------------------------------------------------------------------------
// UMP Endpoint / Function Block Discovery -- same structure as the Pico
// DEVICE-role build (examples/midi_bridge/pico/main.cpp). Without answering
// these Stream messages (MT=0xF), macOS/Windows CoreMIDI-class hosts never
// finish claiming a native-UMP USB device as a real MIDI endpoint -- it
// stays enumerated at the USB level but invisible to MIDI applications.
// ---------------------------------------------------------------------------

// Reply to a host's UMP Endpoint Discovery request (Stream message, MT=0xF).
static void midiendpoint(uint8_t majVer, uint8_t minVer, uint8_t filter)
{
    (void) majVer;
    (void) minVer;

    if (filter & 0x1) {
        std::array<uint32_t, 4> UMP = UMPMessage::mtFMidiEndpointInfoNotify(
                1, true, true, false, false);
        tud_ump_write_hton(kUmpItf, UMP.data(), 4);
    }

    if (filter & 0x2) {
        std::array<uint32_t, 4> UMP = UMPMessage::mtFMidiEndpointDeviceInfoNotify(
                {DEVICE_MFRID}, {DEVICE_FAMID}, {DEVICE_MODELID}, {DEVICE_VERSIONID});
        tud_ump_write_hton(kUmpItf, UMP.data(), 4);
    }

    if (filter & 0x4) {
        const char *name       = effectiveName();
        int         nameLength = static_cast<int>(strlen(name));
        for (uint8_t offset = 0; offset < nameLength; offset += 14) {
            std::array<uint32_t, 4> UMP = UMPMessage::mtFMidiEndpointTextNotify(
                    MIDIENDPOINT_NAME_NOTIFICATION, offset, (uint8_t *) name, nameLength);
            tud_ump_write_hton(kUmpItf, UMP.data(), 4);
        }
    }
}

// Reply to a host's UMP Function Block Discovery request. This bridge
// exposes a single bidirectional function block covering the one UMP group
// carried over the network session.
static void functionblock(uint8_t fbIdx, uint8_t filter)
{
    if (fbIdx != 0 && fbIdx != 0xFF) return;

    if (filter & 0x1) {
        std::array<uint32_t, 4> UMP = UMPMessage::mtFFunctionBlockInfoNotify(
                0, true, 3 /*bidirectional*/, false /*sender*/, false /*recv*/,
                0 /*firstGroup*/, 1 /*groupLength*/, 0x00 /*midiCISupport*/,
                0 /*isMIDI1: full MIDI 2.0 bandwidth over the network transport*/,
                0 /*maxS8Streams*/);
        tud_ump_write_hton(kUmpItf, UMP.data(), 4);
    }

    if (filter & 0x2) {
        char const *name = "NetworkMIDI2 Bridge";
        std::array<uint32_t, 4> UMP = UMPMessage::mtFFunctionBlockNameNotify(
                0, 0, (uint8_t *) name, strlen(name));
        tud_ump_write_hton(kUmpItf, UMP.data(), 4);
    }
}
#endif // NM2_BRIDGE_USB_HOST

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
    gSessionEstablished = false;
    gHostHeartbeatMs    = 0;
    gDisc.unadvertise();
    gTransport.close();
}

// Common "session is up" message -- printed once a session actually starts,
// identical wording/placement to Pico's main.cpp regardless of role.
static void printBridgeRunning()
{
    printf("Bridge running. Press ESC at any time to re-enter setup.\r\n");
}

static void doBeginHost()
{
    destroySession();
    gIsHost = true;

    char epName[64];
    snprintf(epName, sizeof(epName), "%s-host", effectiveName());

    EndpointInfo info;
    info.setName(epName);
    info.setProductId(kProductId);

    NetworkMidiSession::Callbacks cbs{ onUmp, onStateChange, nullptr };
    gSession = new NetworkMidiSession(gTransport, info, cbs);
    gSession->beginHost(kHostPort, &gDisc);

    LOCK_TCPIP_CORE();
    ip4_addr_t hostIp = *netif_ip4_addr(netif_default);
    UNLOCK_TCPIP_CORE();
    printf("[Host] Listening at %s:%u\r\n", ip4addr_ntoa(&hostIp), kHostPort);

    printBridgeRunning();

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
    snprintf(epName, sizeof(epName), "%s-client", effectiveName());

    EndpointInfo info;
    info.setName(epName);
    info.setProductId(kProductId);

    NetworkMidiSession::Callbacks cbs{ onUmp, onStateChange, nullptr };
    gSession = new NetworkMidiSession(gTransport, info, cbs);
    gSession->beginClient(hostEp, kClientPort);

    printBridgeRunning();

    gLineLen = 0; gLineBuf[0] = '\0';
    gState = AppState::SESSION_RUN;
}

// "Current configuration:" printout -- same heading/field layout and
// "DHCP / link-local" wording as Pico's console_menu.cpp printCurrentConfig(),
// plus one added line noting this board doesn't persist config (see the file
// header comment) since Pico's would otherwise imply a flash write happened.
static void printCurrentConfig()
{
    printf("\r\nCurrent configuration:\r\n");
    printf("  Role:    %s\r\n", gIsHost ? "Host" : "Client");
    printf("  Name:    %s\r\n", effectiveName());
    if (gUseDhcp) {
        printf("  Network: DHCP / link-local\r\n");
    } else {
        printf("  Network: static %s / %s (gw %s, dns %s)\r\n",
               gStaticIp, gStaticNetmask, gStaticGateway, gStaticDns);
    }
    if (!gIsHost && gLastHostEp.isValid()) {
        printf("  Host:    ");
        printIp(gLastHostEp.ipv4);
        printf(":%u\r\n", gLastHostEp.port);
    }
    printf("  (not persisted across reboots on this board)\r\n");
}

// Common entry point into the interactive setup walk -- used by the
// BOOT_PROMPT keypress path and ESC-reenter. Matches Pico's runSetupNow():
// same banner, same first question (Role), with the current value shown as
// the bracketed default exactly like console_menu.cpp's
// "Role -- [C]lient or [H]ost [%s]:" convention. Does not itself tear down
// any existing session -- callers that might have one running do that first
// (see the ESC handler in processCli()).
static void enterSetupNow()
{
    gEnteredSetup = true;
    printf("\r\n--- NetworkMIDI2 Bridge setup ---\r\n");
    printf("Role -- [C]lient or [H]ost [%s]: ", gIsHost ? "H" : "C");
    gState      = AppState::ROLE_SELECT;
    gPromptMs   = gTransport.nowMillis();
    gAutoActive = true;
}

// Accepts a dotted-decimal IPv4 string into `dest`, or -- if `line` is
// blank -- leaves `dest` unchanged (it already holds the value shown as
// the prompt's bracketed default). Returns false (dest untouched) if
// `line` is non-blank but doesn't parse, so the caller can re-prompt the
// same field. Same "keep on Enter, validate otherwise" semantics as
// Pico's console_menu.cpp readIpWithDefault().
static bool applyIpFieldOrKeep(const char *line, char *dest, size_t destSize)
{
    if (line[0] == '\0') return true;
    ip4_addr_t addr;
    if (ip4addr_aton(line, &addr) == 0) return false;
    strncpy(dest, line, destSize - 1);
    dest[destSize - 1] = '\0';
    return true;
}

// Same wording/timing as Pico's console_menu.cpp runClientHostSelect():
// a fixed 4 s DNS-SD browse window, Enter to stop early and select from
// what's found so far, or 'm' + Enter for manual IP entry.
static void startMdnsResolve()
{
    gFoundCount = 0;
    printf("\r\nSearching for NetworkMIDI2 hosts (_midi2._udp) for %u seconds...\r\n",
           (unsigned)(kHostBrowseMs / 1000));
    printf("(Enter at any time to stop early and pick from what's found so far,\r\n"
           " or 'm' + Enter to enter a host IP manually.)\r\n");
    gDisc.browse();
    gBrowseDeadlineMs = gTransport.nowMillis();
    gState            = AppState::MDNS_RESOLVE;
}

// Shared by the timeout and Enter/'m' paths out of MDNS_RESOLVE -- same
// selection prompt as Pico's runClientHostSelect() tail.
static void finishMdnsResolve(bool manual)
{
    gDisc.stopBrowse();

    if (!manual && gFoundCount == 0) {
        printf("No hosts found -- enter one manually.\r\n");
        manual = true;
    }

    if (manual) {
        printf("Host IP: ");
        gState = AppState::CLIENT_IP;
        return;
    }

    printf("Select host [1-%d]: ", gFoundCount);
    gState      = AppState::MDNS_RESOLVE; // reuse: next line is a selection digit
    gPromptMs   = gTransport.nowMillis();
    gAutoActive = true;
}

// Time the current WAIT_NETWORK attempt started -- set by
// applyNetworkAndProceed() (and re-armed by its own DHCP-timeout retry),
// read by vSessionTask's WAIT_NETWORK block.
static uint32_t gNetworkStartMs = 0;

// Brings the netif up per the now-decided gUseDhcp/gStatic* fields and
// moves to WAIT_NETWORK to wait for it to actually be ready before
// proceeding (immediately, for static; once a lease arrives, for DHCP).
// Called from both the interactive setup path (NETWORK_SELECT/STATIC_DNS)
// and the BOOT_PROMPT-timeout "continue with current configuration" path --
// safe to call more than once per boot (e.g. after ESC re-entry): lwIP
// tolerates dhcp_start()/netif_set_addr() being called again on a netif
// that's already up, and NM2_NetifSetStatic() stops any running DHCP
// client first so the two don't fight over the address.
static void applyNetworkAndProceed()
{
    LOCK_TCPIP_CORE();
    bool ok = gUseDhcp ? (NM2_NetifStartDhcp(), true)
                        : NM2_NetifSetStatic(gStaticIp, gStaticNetmask, gStaticGateway, gStaticDns);
    UNLOCK_TCPIP_CORE();

    if (!ok) {
        printf("Invalid static network settings -- falling back to DHCP.\r\n");
        gUseDhcp = true;
        LOCK_TCPIP_CORE();
        NM2_NetifStartDhcp();
        UNLOCK_TCPIP_CORE();
    }

    gNetworkStartMs = gTransport.nowMillis();
    gState          = AppState::WAIT_NETWORK;
}

// Shared tail of both the "just finished interactive setup" and "boot
// gate timed out, continue with current configuration" paths, once the
// network is confirmed up (see WAIT_NETWORK in vSessionTask). Same
// role/host-selection logic as Pico's main.cpp for(;;) loop: Host starts
// listening immediately; Client goes straight to mDNS discovery if this
// was reached via the interactive setup walk (gEnteredSetup), otherwise
// reconnects to whatever host was last used, or stays idle if none.
static void proceedAfterNetworkReady()
{
    if (gIsHost) {
        doBeginHost();
    } else if (gEnteredSetup) {
        startMdnsResolve();
    } else if (gLastHostEp.isValid()) {
        doBeginClient(gLastHostEp);
    } else {
        printf("Client role with no host configured -- not starting a session.\r\n"
               "Press ESC to enter setup and choose one.\r\n");
        gState = AppState::SESSION_RUN;
    }
}

// ---------------------------------------------------------------------------
// handleLine — dispatch the completed input line for the current CLI state
// ---------------------------------------------------------------------------

static void handleLine(const char *line)
{
    const bool blank = (line[0] == '\0');

    switch (gState) {

    case AppState::ROLE_SELECT:
        if (line[0] == 'H' || line[0] == 'h') {
            gIsHost = true;
        } else if (line[0] == 'C' || line[0] == 'c') {
            gIsHost = false;
        } else if (!blank) {
            printf("Please enter 'H' or 'C': ");
            break;
        }
        // Blank (Enter alone) keeps the current gIsHost value, matching
        // Pico's "keep whatever was loaded/default" semantics.
        printf("Network MIDI name [%s]: ", effectiveName());
        gState      = AppState::MDNS_NAME;
        gPromptMs   = gTransport.nowMillis();
        gAutoActive = true;
        break;

    case AppState::MDNS_NAME:
        if (!blank) {
            strncpy(gMdnsName, line, sizeof(gMdnsName) - 1);
            gMdnsName[sizeof(gMdnsName) - 1] = '\0';
            for (char *p = gMdnsName; *p; ++p)
                if (*p == ' ') *p = '-';
        }
        // Blank keeps whatever gMdnsName already is -- effectiveName()
        // supplies the "nxpmidi" fallback if it's still empty.
        printf("Network -- [D]HCP/link-local or [S]tatic IP [%s]: ", gUseDhcp ? "D" : "S");
        gState      = AppState::NETWORK_SELECT;
        gPromptMs   = gTransport.nowMillis();
        gAutoActive = true;
        break;

    case AppState::NETWORK_SELECT:
        if (line[0] == 'S' || line[0] == 's') {
            gUseDhcp = false;
        } else if (line[0] == 'D' || line[0] == 'd') {
            gUseDhcp = true;
        } else if (!blank) {
            printf("Please enter 'D' or 'S': ");
            break;
        }
        // Blank keeps whatever gUseDhcp already is, matching Pico.
        if (gUseDhcp) {
            applyNetworkAndProceed();
        } else {
            printf("  Static IP     [%s]: ", gStaticIp);
            gState      = AppState::STATIC_IP;
            gPromptMs   = gTransport.nowMillis();
            gAutoActive = true;
        }
        break;

    case AppState::STATIC_IP:
        if (!applyIpFieldOrKeep(line, gStaticIp, sizeof(gStaticIp))) {
            printf("Not a valid IPv4 address, try again.\r\n  Static IP     [%s]: ", gStaticIp);
            break;
        }
        printf("  Subnet mask   [%s]: ", gStaticNetmask);
        gState      = AppState::STATIC_NETMASK;
        gPromptMs   = gTransport.nowMillis();
        gAutoActive = true;
        break;

    case AppState::STATIC_NETMASK:
        if (!applyIpFieldOrKeep(line, gStaticNetmask, sizeof(gStaticNetmask))) {
            printf("Not a valid IPv4 address, try again.\r\n  Subnet mask   [%s]: ", gStaticNetmask);
            break;
        }
        printf("  Gateway       [%s]: ", gStaticGateway);
        gState      = AppState::STATIC_GATEWAY;
        gPromptMs   = gTransport.nowMillis();
        gAutoActive = true;
        break;

    case AppState::STATIC_GATEWAY:
        if (!applyIpFieldOrKeep(line, gStaticGateway, sizeof(gStaticGateway))) {
            printf("Not a valid IPv4 address, try again.\r\n  Gateway       [%s]: ", gStaticGateway);
            break;
        }
        printf("  DNS server    [%s]: ", gStaticDns);
        gState      = AppState::STATIC_DNS;
        gPromptMs   = gTransport.nowMillis();
        gAutoActive = true;
        break;

    case AppState::STATIC_DNS:
        if (!applyIpFieldOrKeep(line, gStaticDns, sizeof(gStaticDns))) {
            printf("Not a valid IPv4 address, try again.\r\n  DNS server    [%s]: ", gStaticDns);
            break;
        }
        applyNetworkAndProceed();
        break;

    case AppState::MDNS_RESOLVE:
        // Reused for the post-browse numbered selection (see
        // finishMdnsResolve()) once gDisc.stopBrowse() has been called --
        // distinguish by whether a selection prompt is pending via gFoundCount
        // combined with the fact this only runs on a completed input line.
        if (gFoundCount > 0 && (line[0] == '\0' || (line[0] >= '1' && line[0] <= '9'))) {
            if (line[0] >= '1' && line[0] < '1' + gFoundCount) {
                int idx = line[0] - '1';
                doBeginClient(gFoundPeers[idx].endpoint);
                break;
            }
        }
        // 'm'/'M' as a whole line (this state is fully line-buffered -- see
        // processCli() -- so unlike a raw single-char check there's no risk
        // of a trailing Enter leaking into the next prompt) matches the
        // "'m' + Enter to enter a host IP manually" text printed above.
        if (line[0] == 'm' || line[0] == 'M') {
            printf("Host IP: ");
            gState = AppState::CLIENT_IP;
            break;
        }
        if (!blank) {
            printf("Select host [1-%d]: ", gFoundCount);
            break;
        }
        printf("Host IP: ");
        gState = AppState::CLIENT_IP;
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
            case AppState::NETWORK_SELECT:
                printf("D  [auto]\r\n");
                handleLine("D");
                break;
            case AppState::STATIC_IP:
            case AppState::STATIC_NETMASK:
            case AppState::STATIC_GATEWAY:
            case AppState::STATIC_DNS:
                printf("[auto -- keep default]\r\n");
                handleLine(""); // blank -> keep the shown default for this field
                break;
            case AppState::MDNS_RESOLVE:
                if (gFoundCount > 0) {
                    printf("1  [auto]\r\n");
                    handleLine("1");  // auto-select first discovered host
                } else {
                    handleLine("");   // go to CLIENT_IP
                }
                break;
            default:
                break;
            }
            return;
        }
    }

    int c = NM2_GetCharNonBlocking();
    if (c < 0) return;

    // ESC re-enters setup in place, any time the bridge is running (session
    // live or idle with none configured yet) -- no MCU reset, matching
    // examples/midi_bridge/pico/main.cpp's kReconfigureKey handling exactly.
    // Checked as a raw byte, before line-buffering, so it takes effect
    // immediately without waiting for Enter. No other key does anything in
    // SESSION_RUN, same as Pico's run loop.
    if (gState == AppState::SESSION_RUN) {
        if (c == kReconfigureKey) {
            printf("ESC pressed -- re-entering setup...\r\n");
            destroySession(); // best-effort; already bounded to ~200 ms, see its comment
            enterSetupNow();
        }
        return;
    }

    // Real character received — cancel auto-start countdown.
    gAutoActive = false;
    gPromptMs   = 0;

    if (c == '\r' || c == '\n') {
        gLineBuf[gLineLen] = '\0';
        printf("\r\n");
        handleLine(gLineBuf);
        gLineLen = 0;
        return;
    }

    if ((c == 8 || c == 127) && gLineLen > 0) {
        --gLineLen;
        printf("\b \b");
        return;
    }

    // Only accept printable ASCII into the line buffer -- matches Pico's
    // readLine() guard (console_menu.cpp) exactly. Without this, ESC pressed
    // while a text prompt (e.g. "Host IP:") is active would silently corrupt
    // the buffer instead of being ignored (ESC only does something in
    // SESSION_RUN, handled above).
    if (c >= 0x20 && c < 0x7F && gLineLen < static_cast<int>(sizeof(gLineBuf)) - 1) {
        gLineBuf[gLineLen++] = static_cast<char>(c);
        printf("%c", static_cast<char>(c));
    }
}

// ---------------------------------------------------------------------------
// vSessionTask — FreeRTOS task entry point
// ---------------------------------------------------------------------------

extern "C" void vSessionTask(void * /*params*/)
{
#if !NM2_BRIDGE_USB_HOST
    // Respond to the USB host's UMP Endpoint/Function Block Discovery
    // requests, same as the Pico DEVICE-role build and DIN_Bridge. No
    // equivalent in the HOST role -- see onUmp()'s comment.
    UMPHandler.setMidiEndpoint(midiendpoint);
    UMPHandler.setFunctionBlock(functionblock);
#endif

    printf("\r\n=== Network MIDI 2.0 -- NXP FRDM-MCXN947 ===\r\n");
    printf("Product: %s\r\n", kProductId);

    // Boot-time config/gate now happens BEFORE the netif is brought up (same
    // order as Pico's main.cpp: loadBridgeConfig() -> runConfigMenu() ->
    // wiznet_lwip_init(cfg)) -- Network mode (DHCP vs. static) is one of the
    // things the setup walk below can change, so it has to be decided before
    // NM2_NetifStartDhcp()/NM2_NetifSetStatic() ever runs, not after like
    // this file used to (DHCP-only, started unconditionally at board init).
    printCurrentConfig();
    printf("\r\nPress any key within %u seconds to enter setup "
           "(or press ESC any time later while the bridge is running)...\r\n",
           (unsigned)(kSetupPromptTimeoutMs / 1000));
    gBootDeadlineMs = gTransport.nowMillis();
    gState          = AppState::BOOT_PROMPT;

    // -----------------------------------------------------------------------
    // Main loop — 1 kHz cadence (vTaskDelay 1 ms). Runs forever, matching
    // Pico's main.cpp run loop -- there is no "session closed, run again?"
    // state; ESC is the only way back into setup, same as Pico.
    // -----------------------------------------------------------------------
    for (;;) {

        // ---- Boot-time "press any key to enter setup" gate ----
        // Same 3 s window/wording as Pico's console_menu.cpp runConfigMenu().
        // Handled as its own raw-byte check, not through processCli()'s line
        // buffer -- "any key" should count immediately, not require Enter.
        if (gState == AppState::BOOT_PROMPT) {
            int c = NM2_GetCharNonBlocking();
            if (c >= 0) {
                enterSetupNow();
            } else if (gTransport.nowMillis() - gBootDeadlineMs >= kSetupPromptTimeoutMs) {
                printf("Continuing with the above configuration.\r\n");
                applyNetworkAndProceed();
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // ---- Bring the netif up per the decided config; wait for it ----
        // Entered from applyNetworkAndProceed() (either the BOOT_PROMPT
        // timeout path above, or the interactive setup walk's
        // NETWORK_SELECT/STATIC_DNS -- see handleLine()). For a static
        // address this is ready on the very next tick (NM2_NetifSetStatic()
        // already brought the netif up synchronously); for DHCP this polls
        // the same way the old WAIT_DHCP state did.
        if (gState == AppState::WAIT_NETWORK) {
            static uint32_t lastHeartbMs = 0;

            LOCK_TCPIP_CORE();
            bool ready = gUseDhcp
                ? (netif_is_up(netif_default) && dhcp_supplied_address(netif_default))
                : netif_is_up(netif_default);
            bool linkUp     = netif_is_link_up(netif_default);
            ip4_addr_t myIp = *netif_ip4_addr(netif_default);
            UNLOCK_TCPIP_CORE();

            if (ready) {
                printf("IP: %s  (%s)\r\n", ip4addr_ntoa(&myIp), gUseDhcp ? "DHCP" : "static");
                proceedAfterNetworkReady();
            } else {
                uint32_t now = gTransport.nowMillis();
                // Print link/DHCP state every 5 s
                if (now - lastHeartbMs >= 5000) {
                    printf("  [link %s, %s in progress...]\r\n",
                           linkUp ? "UP" : "DOWN", gUseDhcp ? "DHCP" : "network setup");
                    lastHeartbMs = now;
                }
                if (gUseDhcp && now - gNetworkStartMs >= kDhcpTimeoutMs) {
                    printf("\r\n[warn] DHCP timeout — check Ethernet cable.\r\n"
                           "Retrying...\r\n");
                    gNetworkStartMs = now;
                    LOCK_TCPIP_CORE();
                    NM2_NetifStartDhcp(); // safe to call again -- resets/retries
                    UNLOCK_TCPIP_CORE();
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // ---- mDNS browse poll (client) ----
        if (gState == AppState::MDNS_RESOLVE && gFoundCount == 0) {
            DiscoveredPeer peer{};
            if (gDisc.nextDiscovered(peer)) {
                if (gFoundCount < kMaxFoundPeers) {
                    gFoundPeers[gFoundCount++] = peer;
                    // Same per-host format as Pico's runClientHostSelect().
                    printf("  [%d] %s  ", gFoundCount, peer.epName);
                    printIp(peer.endpoint.ipv4);
                    printf(":%u\r\n", peer.endpoint.port);
                }
            }
            if (gTransport.nowMillis() - gBrowseDeadlineMs >= kHostBrowseMs) {
                finishMdnsResolve(false);
            }
        }

#if NM2_BRIDGE_USB_HOST
        // ---- USB MIDI 2.0 (UMP) inbound: raw pass-through to network ----
        // No Endpoint/Function Block Discovery handling here -- ump_host.cpp
        // doesn't implement UMP Stream messages yet (see onUmp()'s comment),
        // so every word read forwards straight on. Same as
        // examples/midi_bridge/pico/main.cpp's HOST role.
        if (s_usbHostMounted) {
            uint32_t umpBuf[4];
            uint16_t wc = tuh_ump_read_ntoh(s_usbHostDaddr, s_usbHostItfNum, umpBuf, 4);
            if (wc > 0 && gSession && gSession->state() == SessionState::Established) {
                if (!gSession->sendUmp(umpBuf, wc)) {
                    printf("[nm2] sendUmp dropped %u word(s) (queue full)\r\n",
                           (unsigned)wc);
                }
            }
        }
#else
        // ---- USB MIDI 2.0 (UMP) inbound: drain + discovery + forward ----
        // Runs every tick regardless of session state -- a USB host performs
        // its UMP Endpoint/Function Block Discovery handshake immediately
        // after enumeration, long before any NetworkMIDI2 client connects.
        // Gating this on SessionState::Established (as an earlier version of
        // this file did) meant tud_ump_read_ntoh() was never called and the
        // discovery Stream messages were never drained/answered, so
        // CoreMIDI-class hosts never finished claiming the device as a real
        // MIDI endpoint. Only the *forward-to-network* step below is gated
        // on having an Established session -- matching the Pico DEVICE-role
        // build's main loop structure.
        if (tud_ump_n_mounted(kUmpItf) && tud_ump_n_available(kUmpItf)) {
            uint32_t umpBuf[4];
            uint8_t  wc = tud_ump_read_ntoh(kUmpItf, umpBuf, 4);
            if (wc > 0) {
                for (uint8_t i = 0; i < wc; ++i) {
                    // Endpoint/Function Block Discovery Stream messages are
                    // handled here; everything else (Channel Voice, etc.)
                    // passes straight through to the network session.
                    UMPHandler.processUMP(umpBuf[i]);
                }

                // UMP Stream messages (Message Type 0xF) are answered
                // locally above via midiendpoint()/functionblock(); they are
                // USB<->host session-management traffic, not MIDI data, and
                // must not also be relayed onto the NetworkMIDI2 session.
                uint8_t messageType = (umpBuf[0] >> 28) & 0xF;
                if (messageType != 0xF && gSession &&
                    gSession->state() == SessionState::Established) {
                    if (!gSession->sendUmp(umpBuf, wc)) {
                        printf("[nm2] sendUmp dropped %u word(s) (queue full)\r\n",
                               (unsigned)wc);
                    }
                }
            }
        }
#endif // NM2_BRIDGE_USB_HOST

        // ---- Session tick ----
        if (gSession) {
            gSession->tick();
            SessionState s = gSession->state();

            if (gIsHost && s == SessionState::Idle && !gSessionEstablished &&
                gState == AppState::SESSION_RUN) {
                uint32_t now = gTransport.nowMillis();
                if (now - gHostHeartbeatMs >= 4000) {
                    LOCK_TCPIP_CORE();
                    ip4_addr_t hbIp = *netif_ip4_addr(netif_default);
                    UNLOCK_TCPIP_CORE();
                    printf("[Host] Still waiting at %s:%u  rx=%u\r\n",
                           ip4addr_ntoa(&hbIp), kHostPort,
                           (unsigned)gTransport.rxPackets());
                    gHostHeartbeatMs = now;
                }
            }
        }

        processCli();

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
