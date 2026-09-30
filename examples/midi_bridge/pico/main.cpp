//
// UUT_NetworkMIDI2_Bridge -- bridges Network MIDI 2.0 (UDP, via a Wiznet
// W5500 Ethernet expansion board on the UUT J13 header) to USB MIDI 2.0
// (tusb_ump). Structured the same way as UUT/DIN_Bridge: a tight
// tud_task()-driven poll loop with no RTOS, just with a NetworkMIDI2
// session standing in for the DIN UART.
//
// Two USB roles, selected at build time via NM2_BRIDGE_USB_ROLE (see
// CMakeLists.txt):
//   DEVICE (default) -- the bridge presents as a USB MIDI device to a host
//     computer/DAW, answering Endpoint/Function Block Discovery locally.
//   HOST -- the bridge is itself the USB host, bridging to a directly- or
//     hub-attached USB MIDI device via the tusb_ump Host driver's current
//     state (as developed/tested in UUT/USB_Host_UMP_Test -- see that
//     app's README.md Known Issues for open caveats). That driver doesn't
//     implement UMP Stream-message discovery yet, so the HOST role is a
//     raw UMP pass-through in both directions, no discovery handling.
// NM2_BRIDGE_USB_HOST (0 or 1, set by CMakeLists.txt from that option)
// selects between the two throughout this file.
//

#include "pico/stdio.h"
#include "pico/stdio/driver.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "hardware/clocks.h"

// Wiznet W5500 + lwIP bring-up (vendored/customized, see wiznet_port/).
#include "port_common.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "w5x00_spi.h"
#include "w5x00_lwip.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"

// NetworkMIDI2 session (binary distribution, lwIP transport).
#include "networkmidi2/NetworkMidiSession.h"
#include "networkmidi2/Ump.h"
#include "networkmidi2/Types.h"
#include "LwipUdpTransport.h"
#include "LwipMdnsDiscovery.h"

// Runtime configuration (role, name, network settings) + boot-time setup
// menu -- see issue #17. Replaces the compile-time #defines this file used
// to have for NM2_USE_DHCP / NM2_STATIC_* / NM2_BRIDGE_ROLE_CLIENT.
#include "bridge_config.h"
#include "console_menu.h"

// USB MIDI 2.0 (tusb_ump). DEVICE role (default): + UMP Endpoint/Function
// Block Discovery, same as DIN_Bridge. HOST role (NM2_BRIDGE_USB_ROLE=HOST,
// see CMakeLists.txt): uses the tusb_ump Host driver instead -- as of this
// driver's current state it does NOT implement the UMP Stream-message
// handshake (Endpoint/Function Block Discovery) at all (see
// lib/tusb_ump/ump_host.h's scope notes), so unlike the DEVICE role this
// bridge cannot answer or issue discovery over USB yet -- it's a raw UMP
// pass-through in both directions, matching UUT/USB_Host_UMP_Test.
#include "tusb.h"
#if NM2_BRIDGE_USB_HOST
#include "host/hcd.h"
#include "ump_host.h"
#else
#include "ump_device.h"
#include "include/umpProcessor.h"
#include "include/umpMessageCreate.h"
#endif

using namespace networkmidi2;

// ---------------------------------------------------------------------------
// Network configuration
// ---------------------------------------------------------------------------
// Role, device name, and DHCP-vs-static network settings are runtime
// configuration now (see bridge_config.h/console_menu.h, issue #17) rather
// than compile-time #defines -- gBridgeConfig is loaded from flash (or
// defaulted) and optionally edited via the boot-time setup menu in main().
static BridgeConfig gBridgeConfig;

#define NM2_SESSION_PORT 5004 // Network MIDI 2.0's recommended default port
static uint16_t NM2_CLIENT_LOCAL_PORT = 5005;

// How long to wait for a DHCP lease before giving up and running without a
// session. Matches the NXP bridge's kDhcpTimeoutMs.
#define NM2_DHCP_TIMEOUT_MS 15000

// Pressing this key at any time while the bridge is running re-enters the
// setup menu in place -- USB and the W5500/lwIP netif both stay up
// throughout, so missing the boot-time setup window (or wanting to change
// something) doesn't require a physical power cycle or USB re-enumeration.
// This used to call watchdog_reboot(), which dropped USB entirely and, back
// when DEVICE role's console rode a USB CDC-ACM interface, took the console
// down along with it -- by the time the console port re-enumerated on the
// host the 3s boot-setup window had already passed, effectively impossible
// to reach setup from a USB-only console. See the restart loop in main()
// below.
constexpr int kReconfigureKey  = 0x1B; // ESC
constexpr int kDiagnosticsKey  = 'd';  // dump session counters without disturbing the session

// Receive processing per tick, and ticks per main-loop pass (see the tick
// loop in main()): 32 x 16 = up to 512 datagrams a pass, the W5500 emptied
// every 32.
constexpr unsigned kRxDatagramsPerSlice = 32;
constexpr unsigned kMaxRxSlicesPerPass  = 16;

// Minimal UMP Endpoint Discovery identity, matching DIN_Bridge -- AmeNote
// does not have a registered SysEx manufacturer ID, so this uses the
// reserved "educational/non-commercial" prefix (0x7D) per the MIDI
// Association spec.
#define DEVICE_MFRID 0x7D, 0x00, 0x00
#define DEVICE_FAMID 0x00, 0x00
#define DEVICE_MODELID 0x00, 0x00
#define DEVICE_VERSIONID 0, 1, 0, 0

#if !NM2_BRIDGE_USB_HOST
// Prints whether/when the device-side stack actually reaches
// SET_CONFIGURATION -- tells apart "device never got configured" from "host
// got stuck matching interface drivers after configuration succeeded" when
// the composite MIDI+CDC device appears to hang on enumeration.
extern "C" void tud_mount_cb(void) { printf("[USB] tud_mount_cb -- configured\n"); }
extern "C" void tud_umount_cb(void) { printf("[USB] tud_umount_cb -- unconfigured\n"); }
#endif

// Defined in wiznet_port/w5x00_lwip.c.
extern uint8_t mac[6];

// ---------------------------------------------------------------------------
// lwIP / W5500 state
// ---------------------------------------------------------------------------
static struct netif g_netif;
#define SOCKET_MACRAW 0

// ---------------------------------------------------------------------------
// NetworkMIDI2 session state
// ---------------------------------------------------------------------------
static LwipUdpTransport nm2Transport;
static NetworkMidiSession *nm2Session = nullptr;

#if NM2_BRIDGE_USB_HOST
// ---------------------------------------------------------------------------
// HOST role: track the single currently-mounted USB MIDI device. This
// bridge assumes one downstream USB MIDI device at a time (matching the
// original DEVICE role's implicit single-device semantics, `tud_ump_n_
// mounted(0)`) -- ump_host.cpp itself supports several simultaneously (see
// UUT/USB_Host_UMP_Test's MAX_MOUNTED_UMP table) if this ever needs to
// extend to more than one.
// ---------------------------------------------------------------------------
static bool s_usbHostMounted = false;
static uint8_t s_usbHostDaddr = 0;
static uint8_t s_usbHostItfNum = 0;

// Invoked when a UMP interface finishes enumeration (ump_host.cpp).
void tuh_ump_mount_cb(uint8_t daddr, uint8_t itf_num) {
    printf("[%llu] USB HOST: UMP device mounted daddr=%u itf_num=%u\n", time_us_64(), daddr, itf_num);
    s_usbHostMounted = true;
    s_usbHostDaddr   = daddr;
    s_usbHostItfNum  = itf_num;
}

void tuh_ump_umount_cb(uint8_t daddr, uint8_t itf_num) {
    printf("[%llu] USB HOST: UMP device unmounted daddr=%u itf_num=%u\n", time_us_64(), daddr, itf_num);
    if (s_usbHostMounted && s_usbHostDaddr == daddr && s_usbHostItfNum == itf_num) {
        s_usbHostMounted = false;
    }
}

#else // DEVICE role (original behavior)

umpProcessor UMPHandler;

void midiendpoint(uint8_t majVer, uint8_t minVer, uint8_t filter);
void functionblock(uint8_t fbIdx, uint8_t filter);

// Diagnostic: log every alt-setting change, matching DIN_Bridge.
void tud_ump_set_itf_cb(uint8_t itf, uint8_t alt) {
    printf("[%llu] ALT_SET itf=%d alt=%d (mVersion=%d)\n", time_us_64(), itf, alt, alt + 1);
}

// Reply to a host's UMP Endpoint Discovery request (Stream message, MT=0xF).
// Same pattern as DIN_Bridge/main.cpp.
void midiendpoint(uint8_t majVer, uint8_t minVer, uint8_t filter) {
    (void) majVer;
    (void) minVer;

    if (filter & 0x1) {
        std::array<uint32_t, 4> UMP = UMPMessage::mtFMidiEndpointInfoNotify(
                1, true, true, false, false);
        tud_ump_write_hton(0, UMP.data(), 4);
    }

    if (filter & 0x2) {
        std::array<uint32_t, 4> UMP = UMPMessage::mtFMidiEndpointDeviceInfoNotify(
                {DEVICE_MFRID}, {DEVICE_FAMID}, {DEVICE_MODELID}, {DEVICE_VERSIONID});
        tud_ump_write_hton(0, UMP.data(), 4);
    }

    if (filter & 0x4) {
        int nameLength = strlen(gBridgeConfig.name);
        for (uint8_t offset = 0; offset < nameLength; offset += 14) {
            std::array<uint32_t, 4> UMP = UMPMessage::mtFMidiEndpointTextNotify(
                    MIDIENDPOINT_NAME_NOTIFICATION, offset, (uint8_t *) gBridgeConfig.name, nameLength);
            tud_ump_write_hton(0, UMP.data(), 4);
        }
    }
}

// Reply to a host's UMP Function Block Discovery request. This bridge
// exposes a single bidirectional function block covering the one UMP group
// carried over the network session.
void functionblock(uint8_t fbIdx, uint8_t filter) {
    if (fbIdx != 0 && fbIdx != 0xFF) return;

    if (filter & 0x1) {
        std::array<uint32_t, 4> UMP = UMPMessage::mtFFunctionBlockInfoNotify(
                0, true, 3 /*bidirectional*/, false /*sender*/, false /*recv*/,
                0 /*firstGroup*/, 1 /*groupLength*/, 0x00 /*midiCISupport*/,
                0 /*isMIDI1: full MIDI 2.0 bandwidth over the network transport*/,
                0 /*maxS8Streams*/);
        tud_ump_write_hton(0, UMP.data(), 4);
    }

    if (filter & 0x2) {
        char const *name = "NetworkMIDI2 Bridge";
        std::array<uint32_t, 4> UMP = UMPMessage::mtFFunctionBlockNameNotify(
                0, 0, (uint8_t *) name, strlen(name));
        tud_ump_write_hton(0, UMP.data(), 4);
    }
}
#endif // NM2_BRIDGE_USB_HOST

static uint32_t s_rxFrames      = 0;
static uint32_t s_rxPbufFail    = 0;  ///< PBUF_POOL exhausted -- frame discarded
static uint32_t s_rxInputFail   = 0;  ///< lwIP refused the frame
static uint32_t s_rxPollMaxRun  = 0;  ///< most frames drained in one poll; at
                                      ///< kMaxFramesPerPoll the W5500 still had
                                      ///< more queued and we left it there

// Dump the session's loss-recovery / inbound-accounting counters on demand.
// These already existed in NetworkMidiSession but this build never printed
// them, which is why a net->USB SysEx arriving complete-but-short with a bad
// checksum looked like it had no device-side explanation at all.
// Bye reason text, M2-124-UM v1.0.1 section 6.16 Table 27.
//
// Worth having in full rather than the five codes this example used to know:
// the previous list mislabelled 0x01 as PowerDown (the spec says "user
// terminated session"), 0x02 as TooManySessions (the spec says Power Down),
// and treated 0x10 as Timeout, which is not a defined reason at all. A peer's
// Bye is often the only account of why a session ended, so decoding it wrongly
// sends the reader after the wrong fault.
static const char *nm2ByeReasonText(uint8_t r)
{
    switch (r) {
    case 0x00: return "Unknown or Undefined";
    case 0x01: return "User terminated session";
    case 0x02: return "Power Down";
    case 0x03: return "Too Many Missing UMP Packets";
    case 0x04: return "Timeout";
    case 0x05: return "Session Not Established";
    case 0x06: return "No Pending Session";
    case 0x07: return "Protocol Error";
    case 0x40: return "Invitation Failed: too many open sessions";
    case 0x41: return "Invitation w/ Auth Rejected: no prior invitation";
    case 0x42: return "Invitation Rejected: user did not accept";
    case 0x43: return "Invitation Rejected: authentication failed";
    case 0x44: return "Invitation Rejected: username not found";
    case 0x45: return "No Matching Authentication Method";
    case 0x80: return "Invitation Canceled";
    default:   return "unknown (not in Table 27)";
    }
}

static void printSessionDiagnostics() {
    if (!nm2Session) { printf("NM2 diag: no session\n"); return; }
    NetworkMidiSession::Diagnostics d = nm2Session->diagnostics();
    printf("[%llu] NM2 diag: in cmds %lu ump %lu dup %lu empty %lu malformed %lu "
           "truncated %lu trailing %lu\n",
           time_us_64(), (unsigned long) d.cmdsRecv, (unsigned long) d.umpDataRecv,
           (unsigned long) d.umpDupDropped, (unsigned long) d.umpEmptyRecv,
           (unsigned long) d.umpMalformed,
           (unsigned long) d.umpTruncated, (unsigned long) d.pktTrailingBytes);
    printf("           in-order: gapsSkipped %lu lateDropped %lu heldMax %lu"
           " | sysex cutOff %lu orphans %lu\n",
           (unsigned long) d.gapsSkipped, (unsigned long) d.umpLateDropped,
           (unsigned long) d.heldMax, (unsigned long) d.sysexCutOff,
           (unsigned long) d.sysexOrphans);
    printf("           recovery: gapsDropped %lu gapScanMax %lu rtx.req tx %lu rx %lu "
           "rtx.err tx %lu rx %lu reset tx %lu rx %lu\n",
           (unsigned long) d.gapsDropped, (unsigned long) d.gapScanMax,
           (unsigned long) d.retransmitReqSent, (unsigned long) d.retransmitReqRecv,
           (unsigned long) d.retransmitErrSent, (unsigned long) d.retransmitErrRecv,
           (unsigned long) d.sessionResetSent, (unsigned long) d.sessionResetRecv);
    printf("           w5500 rx: frames %lu pbufFail %lu inputFail %lu maxPerPoll %lu"
           " | udp rx %lu ringDrop %lu\n",
           (unsigned long) s_rxFrames, (unsigned long) s_rxPbufFail,
           (unsigned long) s_rxInputFail, (unsigned long) s_rxPollMaxRun,
           (unsigned long) nm2Transport.rxPackets(), (unsigned long) nm2Transport.rxDropped());
    printf("           liveness: ping tx %lu rx %lu reply.rx %lu | bye tx %lu rx %lu "
           "| timeouts %lu | max.silence %lu ms\n",
           (unsigned long) d.pingSent, (unsigned long) d.pingRecv,
           (unsigned long) d.pingReplyRecv, (unsigned long) d.byeSent,
           (unsigned long) d.byeRecv, (unsigned long) d.timeouts,
           (unsigned long) d.maxRxSilenceMs);
}

// Network -> USB drops were previously silent on this build: onNetworkUmp()
// simply returned when the USB TX FIFO could not take a whole message, so a
// dropped SysEx continuation showed up only at the far end, as a complete but
// short message with a bad checksum, while every device-side counter read
// zero. Report it the same aggregated way sendUmp() drops are reported.
static uint32_t s_usbTxDroppedWords = 0;
static uint32_t s_usbTxDroppedMsgs  = 0;
static uint32_t s_lastUsbTxDropMs   = 0;

static uint32_t s_usbNoDeviceWords = 0;
static uint32_t s_usbNoDeviceMsgs  = 0;
static uint32_t s_lastNoDeviceMs   = 0;

static void noteUsbNoDevice(size_t wordCount) {
    s_usbNoDeviceWords += (uint32_t) wordCount;
    s_usbNoDeviceMsgs++;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_lastNoDeviceMs >= 1000) {
        s_lastNoDeviceMs = now;
        printf("[%llu] NM2 net->USB: %lu message(s) / %lu word(s) discarded in "
               "the last ~1s (no USB device mounted)\n",
               time_us_64(), (unsigned long) s_usbNoDeviceMsgs,
               (unsigned long) s_usbNoDeviceWords);
        s_usbNoDeviceWords = 0;
        s_usbNoDeviceMsgs  = 0;
    }
}

static void noteUsbTxDropped(size_t wordCount) {
    s_usbTxDroppedWords += (uint32_t) wordCount;
    s_usbTxDroppedMsgs++;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_lastUsbTxDropMs >= 1000) {
        s_lastUsbTxDropMs = now;
        printf("[%llu] NM2 net->USB: %lu message(s) / %lu word(s) dropped in the "
               "last ~1s (USB TX FIFO full)\n",
               time_us_64(), (unsigned long) s_usbTxDroppedMsgs,
               (unsigned long) s_usbTxDroppedWords);
        s_usbTxDroppedWords = 0;
        s_usbTxDroppedMsgs  = 0;
    }
}

// Called synchronously from within nm2Session->tick() for each UMP message
// received over the network. Per NetworkMIDI2's docs this must not block or
// call sendUmp()/close() -- tuh_ump_write_hton()/tud_ump_write_hton() are
// non-blocking FIFO writes, matching how DIN_Bridge forwards DIN bytes to
// USB directly. The one exception is usbTxRoomFor()'s bounded wait below.
static networkmidi2::SysExDropGate s_netToUsbGate;   // see networkmidi2/Ump.h

// The USB TX FIFO drains only when the USB stack runs: each completed
// transfer starts the next one from tud_task()/tuh_task(). One network
// delivery can carry a whole large SysEx (1 KB of SysEx7 is 1368 bytes of
// UMP), so the FIFO fills partway through it while nothing drains it. When a
// packet does not fit, run the USB stack for up to kUsbTxWaitUs to let it
// drain, instead of dropping the rest of the message.
//
// The wait is bounded, and is skipped once one has run out without room (the
// device is not reading, or not keeping up), until the FIFO takes a packet
// again -- so a stalled device costs one wait, not one per packet.
#ifndef NM2_USB_TX_WAIT_US
#define NM2_USB_TX_WAIT_US 2000   // 0 disables the wait
#endif
static constexpr uint32_t kUsbTxWaitUs = NM2_USB_TX_WAIT_US;
static bool s_usbTxStalled = false;

static bool usbTxRoomFor(uint8_t pw) {
#if NM2_BRIDGE_USB_HOST
    auto writeable = [] { return tuh_ump_writeable(s_usbHostDaddr, s_usbHostItfNum); };
#else
    auto writeable = [] { return tud_ump_n_writeable(0); };
#endif
    if (writeable() >= pw) { s_usbTxStalled = false; return true; }
    if (s_usbTxStalled) return false;
    const uint64_t deadline = time_us_64() + kUsbTxWaitUs;
    while (time_us_64() < deadline) {
#if NM2_BRIDGE_USB_HOST
        tuh_task();
#else
        tud_task();
#endif
        if (writeable() >= pw) return true;
    }
    s_usbTxStalled = true;
    return false;
}

static void onNetworkUmp(void *ctx, const uint32_t *words, size_t wordCount) {
    (void) ctx;
    // Ask before writing. The *_write_hton() calls report only what they
    // managed to take, so a partially accepted message would go out torn in
    // half -- worse than dropping it, because the far end would reassemble the
    // remains into something that was never sent.
#if NM2_BRIDGE_USB_HOST
    // Packet by packet: one delivery may carry several messages, and the
    // decision to abandon a SysEx is per message. Dropping a lone Continue
    // leaves the receiver assembling a message that ends short -- see
    // networkmidi2/Ump.h.
    for (size_t off = 0; off < wordCount; ) {
        const uint8_t pw = networkmidi2::umpWordCount(words[off]);
        if (pw == 0 || off + pw > wordCount) break;
        const uint32_t *pkt = words + off;
        off += pw;

        if (s_netToUsbGate.suppress(pkt[0])) {
            noteUsbTxDropped(pw);
            uint32_t term[2];
            if (s_netToUsbGate.terminatorFor(pkt[0], term) == 2 && tuh_ump_writeable(s_usbHostDaddr, s_usbHostItfNum) >= 2) {
                tuh_ump_write_hton(s_usbHostDaddr, s_usbHostItfNum, term, (uint8_t) 2);
            }
            continue;
        }
        if (!usbTxRoomFor(pw)) {
            noteUsbTxDropped(pw);
            s_netToUsbGate.noteRefused(pkt[0]);
            continue;
        }
        tuh_ump_write_hton(s_usbHostDaddr, s_usbHostItfNum, const_cast<uint32_t *>(pkt), (uint8_t) pw);
        s_netToUsbGate.noteSent(pkt[0]);
    }
#else
    // Packet by packet: one delivery may carry several messages, and the
    // decision to abandon a SysEx is per message. Dropping a lone Continue
    // leaves the receiver assembling a message that ends short -- see
    // networkmidi2/Ump.h.
    for (size_t off = 0; off < wordCount; ) {
        const uint8_t pw = networkmidi2::umpWordCount(words[off]);
        if (pw == 0 || off + pw > wordCount) break;
        const uint32_t *pkt = words + off;
        off += pw;

        if (s_netToUsbGate.suppress(pkt[0])) {
            noteUsbTxDropped(pw);
            uint32_t term[2];
            if (s_netToUsbGate.terminatorFor(pkt[0], term) == 2 && tud_ump_n_writeable(0) >= 2) {
                tud_ump_write_hton(0, term, (uint8_t) 2);
            }
            continue;
        }
        if (!usbTxRoomFor(pw)) {
            noteUsbTxDropped(pw);
            s_netToUsbGate.noteRefused(pkt[0]);
            continue;
        }
        tud_ump_write_hton(0, const_cast<uint32_t *>(pkt), (uint8_t) pw);
        s_netToUsbGate.noteSent(pkt[0]);
    }
#endif
}

// Reports dropped-on-send UMP words (FIFO full / session not established) at
// most once/second, as an aggregate count -- printing one line per drop would
// flood the UART under sustained congestion (the exact condition being
// reported), adding CPU-time logging overhead on top of the drops themselves.
static uint32_t s_umpDroppedWords  = 0;
static uint32_t s_lastDropReportMs = 0;

// Carries an incomplete UMP packet between reads: a USB FIFO is read in words,
// so a read can stop mid-packet, and sending that half packet breaks 7.2.
// Twelve bytes each, not a SysEx buffer -- see networkmidi2/Ump.h.
static networkmidi2::UmpFramer s_usbDevFramer;
#if NM2_BRIDGE_USB_HOST
static networkmidi2::UmpFramer s_usbHostFramer;
#endif

static void noteUmpDropped(uint16_t wordCount) {
    s_umpDroppedWords += wordCount;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_lastDropReportMs >= 1000) {
        s_lastDropReportMs = now;
        printf("[%llu] NM2 sendUmp: %lu word(s) dropped in the last ~1s "
               "(FIFO full or session not established)\n",
               time_us_64(), (unsigned long) s_umpDroppedWords);
        s_umpDroppedWords = 0;
    }
}

static void onNetworkStateChange(void *ctx, SessionState newState) {
    (void) ctx;
    static const char *kStateNames[] = {
            "Idle", "PendingInvitation", "AuthRequired",
            "Established", "PendingReset", "PendingBye",
    };
    printf("[%llu] NM2 session state -> %s\n", time_us_64(), kStateNames[(int) newState]);

    // A drop to Idle is otherwise silent about cause, which makes a host that
    // actively refuses the invitation look identical to one that never
    // answered at all.
    if (newState == SessionState::Idle && nm2Session) {
        uint8_t reason = nm2Session->lastByeReason();
        if (reason != 0xFF) {
            const char *why = nm2ByeReasonText(reason);
            printf("         last Bye from peer: 0x%02X (%s)\n", reason, why);
        }
        printf("         Reply-Pending received this attempt: %u\n",
               (unsigned) nm2Session->replyPendingCount());
    }
}

// ---------------------------------------------------------------------------
// Wiznet W5500 + lwIP netif bring-up
// ---------------------------------------------------------------------------
// False until wiznet_lwip_init() has brought a netif up; guards every path
// that would otherwise touch the W5500 or lwIP on a board where the chip
// never answered.
static bool gNetworkUp = false;

// Returns false if the W5500 never answered, in which case no netif was
// brought up and the caller should skip anything network-dependent. USB is
// already enumerated by this point and stays working either way (issue #19).
static bool wiznet_lwip_init(const BridgeConfig &cfg) {
    wizchip_spi_initialize();
    wizchip_cris_initialize();

    wizchip_reset();
    wizchip_initialize();
    if (!wizchip_check_ok()) {
        printf("W5500 not responding -- continuing without Ethernet.\n");
        return false;
    }

    w5x00_randomize_mac(); // per-board unique MAC -- see w5x00_lwip.h
    setSHAR(mac);
    ctlwizchip(CW_RESET_PHY, 0);

    wiz_NetInfo netInfo = {};
    memcpy(netInfo.mac, mac, 6);
    if (cfg.useDhcp) {
        netInfo.dhcp = NETINFO_DHCP;
    } else {
        netInfo.dhcp = NETINFO_STATIC;
        parseDottedIp(cfg.staticIp, netInfo.ip);
        parseDottedIp(cfg.staticNetmask, netInfo.sn);
        parseDottedIp(cfg.staticGateway, netInfo.gw);
        parseDottedIp(cfg.staticDns, netInfo.dns);
    }
    network_initialize(netInfo);
    print_network_information(netInfo);

    lwip_init();

    if (cfg.useDhcp) {
        netif_add(&g_netif, IP4_ADDR_ANY, IP4_ADDR_ANY, IP4_ADDR_ANY, NULL, netif_initialize, netif_input);
    } else {
        ip4_addr_t ip, sn, gw;
        IP4_ADDR(&ip, netInfo.ip[0], netInfo.ip[1], netInfo.ip[2], netInfo.ip[3]);
        IP4_ADDR(&sn, netInfo.sn[0], netInfo.sn[1], netInfo.sn[2], netInfo.sn[3]);
        IP4_ADDR(&gw, netInfo.gw[0], netInfo.gw[1], netInfo.gw[2], netInfo.gw[3]);
        netif_add(&g_netif, &ip, &sn, &gw, NULL, netif_initialize, netif_input);
    }
    g_netif.name[0] = 'e';
    g_netif.name[1] = '0';

    netif_set_link_callback(&g_netif, netif_link_callback);
    netif_set_status_callback(&g_netif, netif_status_callback);

    if (socket(SOCKET_MACRAW, Sn_MR_MACRAW, NM2_SESSION_PORT, 0x00) < 0) {
        printf("MACRAW socket open failed\n");
    }

    netif_set_default(&g_netif);
    netif_set_link_up(&g_netif);
    netif_set_up(&g_netif);

    if (cfg.useDhcp) {
        dhcp_start(&g_netif);
    }
    gNetworkUp = true;
    return true;
}

// Pump any pending Ethernet frames from the W5500 up into lwIP. Must be
// called every loop iteration alongside sys_check_timeouts() since this
// project runs lwIP in NO_SYS=1 (no tcpip thread) -- same pattern as the
// RP2040-HAT-LWIP-C dhcp_dns example this was adapted from.
// Inbound frame accounting. Both of the drops below were previously silent,
// which is why inbound loss showed up only as a session-level gap with no
// device-side explanation.
static void wiznet_lwip_poll() {
    if (!gNetworkUp) return; // no W5500, no MACRAW socket to read from

    // Drain up to this many frames per call rather than exactly one.
    //
    // Taking a single frame per main-loop pass was a real source of inbound
    // loss: macOS delivers a SysEx burst as many datagrams back to back, and
    // the W5500's RX buffer is finite, so anything arriving faster than one
    // frame per loop iteration backed up and was overwritten on the chip --
    // invisible here, and visible at the session only as an unrecoverable
    // sequence gap. Bounded so a sustained flood still cannot starve USB
    // servicing or the session tick.
    static constexpr unsigned kMaxFramesPerPoll = 256;
    unsigned drained = 0;

    for (; drained < kMaxFramesPerPoll; ++drained) {
    uint16_t pending = 0;
    getsockopt(SOCKET_MACRAW, SO_RECVBUF, &pending);
    if (pending == 0) break;

    // recv_lwip()'s bounds check compares the incoming frame's declared
    // length against the `len` we pass here -- it must be the actual
    // capacity of packetBuf (sizeof(packetBuf)), NOT `pending` (the total
    // bytes currently queued in the W5500's RX buffer, which can be larger
    // than a single frame and is unrelated to packetBuf's size). Passing
    // `pending` here made the bounds check a no-op and let a full-size
    // Ethernet frame overflow a too-small buffer.
    static uint8_t packetBuf[ETHERNET_FRAME_MAX_SIZE];
    uint16_t pack_len = recv_lwip(SOCKET_MACRAW, packetBuf, sizeof(packetBuf));
    if (pack_len == 0) break;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, pack_len, PBUF_POOL);
    if (p == nullptr) { s_rxPbufFail++; break; }
    pbuf_take(p, packetBuf, pack_len);

    s_rxFrames++;
    LINK_STATS_INC(link.recv);
    if (g_netif.input(p, &g_netif) != ERR_OK) {
        s_rxInputFail++;
        pbuf_free(p);
    }
    }

    if (drained > s_rxPollMaxRun) s_rxPollMaxRun = drained;
}

// Passed to runClientHostSelect() as its network-pump callback -- see the
// comment on that function in console_menu.h for why it's needed there.
static void wiznet_lwip_poll_tick() {
    wiznet_lwip_poll();
    sys_check_timeouts();
}

// Block until the netif actually holds a routable address, not merely until
// the netif exists. gNetworkUp / wiznet_lwip_init()'s return value only mean
// "the W5500 answered and dhcp_start() was called" -- with DHCP the address
// is still 0.0.0.0 for a second or more afterwards.
//
// Starting a session in that window binds the transport to 0.0.0.0 and sends
// invitations from an unusable source address. Acquiring the real lease later
// does NOT repair the already-started attempt: observed on ProtoZOA talking to
// a Windows host, which answered ReplyPending 38 times and then gave up with
// Bye(Normal) while the board sat wedged. The mDNS host-select path already
// waited via LwipMdnsDiscovery::isNetifReady, but the auto-connect path (a
// host already configured, setup menu skipped) bypassed it entirely.
//
// Returns false on timeout, in which case the caller must not start a session.
static void usb_task_tick(); // defined below

static bool waitForNetworkAddress(uint32_t timeoutMs) {
    if (!gNetworkUp) return false;

    absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
    bool announced = false;
    while (!LwipMdnsDiscovery::isNetifReady()) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            printf("\n[warn] No IP address after %lus -- check the Ethernet cable "
                   "or DHCP server.\n", (unsigned long) (timeoutMs / 1000));
            return false;
        }
        if (!announced) {
            printf("Waiting for DHCP lease...\n");
            announced = true;
        }
        wiznet_lwip_poll_tick();
        usb_task_tick(); // keep USB serviced while we wait
        sleep_ms(1);
    }
    return true;
}

// Passed to setConsolePump(): keeps the USB stack serviced while the
// boot-time menus (over UART0, both roles -- see CMakeLists.txt) wait for
// input, so USB MIDI enumeration/control transfers keep progressing even
// while blocked on a keystroke there. Both roles' console pump is now the
// ONLY driver of tud_task()/tuh_task() outside the main run loop:
// pico_stdio_usb is disabled entirely for DEVICE role (no CDC interface in
// usb_descriptors.cpp this release, see CMakeLists.txt's comment) so there
// is no longer a background IRQ task competing to call the non-reentrant
// tud_task() concurrently with this one, which is what used to wedge the
// now-removed CDC console's RX path after a host-side reconnect (see git
// log "fix CDC console RX wedging" if that history matters again).
static void usb_task_tick() {
#if NM2_BRIDGE_USB_HOST
    tuh_task();
#else
    tud_task();
#endif
}

int main() {
#if !NM2_BRIDGE_USB_HOST
    // DEVICE role: tusb_init() must run BEFORE stdio_init_all(), not after.
    // pico_enable_stdio_usb's stdio_usb_init() (called from stdio_init_all())
    // only auto-calls tusb_init() itself when PICO_STDIO_USB_ENABLE_TINYUSB_INIT
    // defaults on -- which the SDK does NOT do once the app links tinyusb_device
    // directly (LIB_TINYUSB_DEVICE, true for this build). Instead it takes the
    // `assert(tud_inited())` path and expects the caller to have already
    // initialized TinyUSB -- confirmed by reading stdio_usb.c directly. Getting
    // this order backwards means stdio_usb_init() either asserts (debug builds)
    // or drives an uninitialized USB core (release builds).
    //
    // Respond to the USB host's UMP Endpoint/Function Block Discovery
    // requests, matching DIN_Bridge.
    UMPHandler.setMidiEndpoint(midiendpoint);
    UMPHandler.setFunctionBlock(functionblock);
    // processUMP() calls sendOutSysex with no null check on its Message Type 5
    // (128-bit data / SysEx8) path -- AM_MIDI2.0Lib umpProcessor.cpp, unlike
    // the guarded SysEx7 path -- and an empty std::function throws
    // bad_function_call, which without exceptions is abort(). Every word the
    // USB host sends passes through here, so one misaligned or malformed
    // stream (captured: a host re-sending stale 512-byte-aligned data) parked
    // the whole bridge in _exit. SysEx itself is bridged by the pass-through
    // in the USB drain loop, not by this callback, so a no-op is correct.
    UMPHandler.setSysEx([](struct umpData) {});

    tusb_init();

    // A debugger-issued SWD reset (as used when reflashing over a picoprobe
    // during bench bring-up) resets the RP2040 core/logic but can leave the
    // USB pull-up state change too brief for the host/hub to register as a
    // real detach -- the host then keeps its stale, previously-enumerated
    // descriptor set instead of re-reading the new one. A real power-cycle
    // or cable replug doesn't have this problem; this pulse makes a bare
    // debugger reset behave the same way, so a new descriptor set is
    // reliably picked up without a physical replug.
    tud_disconnect();
    sleep_ms(250);
    tud_connect();
#endif

    stdio_init_all();

    printf("Starting AmeNote ProtoZOA NetworkMIDI2 Bridge\n");
    printf("System clock: %lu MHz\n", (unsigned long) (clock_get_hz(clk_sys) / 1000000u));

    // Issue #19: bring USB up FIRST, before the setup menu, the W5500 and
    // host discovery. The D+ pull-up is only asserted by tusb_init(), so
    // until it runs the USB host does not even see a device attach -- and
    // everything between here and there can stall for seconds or, with no
    // serial console attached, forever. That is why the bridge showed up as
    // nothing at all in Windows Device Manager while working on a macOS
    // bench that always had a console and an Ethernet host present.
    //
    // Doing this first also keeps the board inside the 100mA that USB
    // guarantees before SET_CONFIGURATION: enumeration now completes before
    // wiznet_lwip_init() powers up the Ethernet PHY. DEVICE role now brings
    // USB up even earlier than before (ahead of stdio_init_all() itself), so
    // this guarantee only gets stronger, not weaker.
    //
    // HOST role: everything that waits from here on must keep calling
    // tuh_task(), or the host's enumeration control transfers go unanswered
    // -- see setConsolePump() below and the pumping in console_menu.cpp.

#if NM2_BRIDGE_USB_HOST
    tusb_rhport_init_t host_init = {};
    host_init.role  = TUSB_ROLE_HOST;
    host_init.speed = TUSB_SPEED_AUTO;
    tusb_init(BOARD_TUH_RHPORT, &host_init);

    // RP2040's host controller only notifies TinyUSB of a device via an
    // edge-triggered connect interrupt -- if a device was already plugged
    // in before tusb_init() ran, there's no edge to catch and it's silently
    // never enumerated. Same workaround as UUT/USB_Host_UMP_Test.
    if (hcd_port_connect_status(BOARD_TUH_RHPORT)) {
        printf("USB device already attached at boot -- forcing enumeration\n");
        hcd_event_device_attach(BOARD_TUH_RHPORT, false);
    }
#endif
    setConsolePump(usb_task_tick);

    loadBridgeConfig(gBridgeConfig);
    bool enteredSetup = runConfigMenu(gBridgeConfig);

    bool networkUp = wiznet_lwip_init(gBridgeConfig);

    static LwipMdnsDiscovery mdnsDisc;

    // EndpointInfo (name + product ID) is captured once here -- NetworkMidiSession
    // is non-copyable/non-movable (see NetworkMidiSession.h) and takes it by
    // const-ref at construction, so a name change from the ESC-triggered
    // setup below cannot be applied to the already-constructed `session`
    // without recreating it. Flagged with a printed note where that matters;
    // still a small improvement over the old full-reboot path, which lost
    // USB entirely just to reach setup at all.
    EndpointInfo info;
    info.setName(gBridgeConfig.name);
    info.setProductId("com.amenote.protozoa.nm2-bridge");

    NetworkMidiSession::Callbacks cb;
    cb.ctx           = nullptr;
    cb.onUmp          = onNetworkUmp;
    cb.onStateChange = onNetworkStateChange;

    static NetworkMidiSession session(nm2Transport, info, cb);
    nm2Session = &session;
    // Process received datagrams in slices, emptying the W5500 between them
    // (see the tick loop below): the chip holds ~100 frames, 20-30 ms of a
    // Wi-Fi burst, and an unbounded tick over a full receive ring kept the
    // loop away from it long enough to overflow.
    session.setMaxDatagramsPerTick(kRxDatagramsPerSlice);

    // Outer loop: normally runs exactly once. Re-entered, without a
    // hardware reset, when ESC is pressed inside the run loop below --
    // only the host-select -> session-(re)start sequence repeats each
    // time. USB and the W5500/lwIP netif are brought up once above and
    // stay up across every re-entry; see kReconfigureKey's comment.
    for (;;) {
        if (strncmp(info.name, gBridgeConfig.name, sizeof(info.name)) != 0) {
            printf("Note: Network MIDI name change to \"%s\" needs a power cycle to take "
                   "effect -- this session keeps advertising as \"%s\" for now.\n",
                   gBridgeConfig.name, info.name);
        }

        // Nothing below may run until the netif holds a real address -- both
        // the host-select browse and beginClient()/beginHost() bind to it.
        // No-op on ESC re-entry, since the lease is long since up by then.
        if (networkUp && !waitForNetworkAddress(NM2_DHCP_TIMEOUT_MS)) {
            networkUp = false;
        }

        // Client role: pick a host now that the network is up, either because
        // the user just walked the setup menu or because no host has ever been
        // configured (first boot).
        bool haveClientHost = gBridgeConfig.clientHostIp[0] != '\0';
        if (networkUp && gBridgeConfig.role == BridgeRole::Client &&
            (enteredSetup || !haveClientHost)) {
            haveClientHost = runClientHostSelect(gBridgeConfig, mdnsDisc, wiznet_lwip_poll_tick,
                                                  LwipMdnsDiscovery::isNetifReady, enteredSetup);
        }

        // False when we never got as far as beginClient()/beginHost() -- the
        // session object exists but has no transport bound, so the main loop
        // must not tick() or send into it.
        bool sessionStarted = true;

        if (!networkUp) {
            printf("No Ethernet -- running as a USB MIDI device only.\n");
            sessionStarted = false;
        } else if (gBridgeConfig.role == BridgeRole::Client && !haveClientHost) {
            // Unattended boot with nothing discovered -- stay up as a USB MIDI
            // device (already enumerated) and wait for the user to configure a
            // host rather than dialling 0.0.0.0. See runClientHostSelect().
            printf("Client role with no host configured -- not starting a session.\n"
                   "Press ESC to enter setup and choose one.\n");
            sessionStarted = false;
        } else if (gBridgeConfig.role == BridgeRole::Client) {
            uint8_t hostOctets[4];
            parseDottedIp(gBridgeConfig.clientHostIp, hostOctets);
            UdpEndpoint hostEp;
            hostEp.ipv4 = (uint32_t(hostOctets[0]) << 24) | (uint32_t(hostOctets[1]) << 16) |
                          (uint32_t(hostOctets[2]) << 8) | uint32_t(hostOctets[3]);
            hostEp.port = gBridgeConfig.clientHostPort;
            nm2Session->beginClient(hostEp, NM2_CLIENT_LOCAL_PORT);
        } else {
            nm2Session->beginHost(NM2_SESSION_PORT, &mdnsDisc);
            printf("[Host] Listening at %s:%u\n", ip4addr_ntoa(netif_ip4_addr(&g_netif)), NM2_SESSION_PORT);
        }

        printf("Bridge running. Press ESC at any time to re-enter setup.\n");

        bool reconfigureRequested = false;

        // ------- Loop: pump lwIP/W5500, USB, and the NM2 session -------
        while (true) {
#if NM2_BRIDGE_USB_HOST
            tuh_task();
#else
            tud_task();
#endif

            int pressed = getchar_timeout_us(0);
            if (pressed == kDiagnosticsKey) {
                printSessionDiagnostics();
            }
            if (pressed == kReconfigureKey) {
                printf("ESC pressed -- re-entering setup...\n");
                // Best-effort Bye -- we are not sticking around to pump
                // tick() until the peer's BeReply arrives (about to tear
                // this session down anyway), but this is still strictly
                // better than the old watchdog_reboot(), which gave the
                // peer no notice at all and relied purely on its own
                // session timeout.
                if (sessionStarted) nm2Session->close();
                reconfigureRequested = true;
                break;
            }

            wiznet_lwip_poll();
            sys_check_timeouts();

#if NM2_BRIDGE_USB_HOST
        // USB -> Network: pull decoded UMP words from the attached USB MIDI
        // device and forward each message into the NetworkMIDI2 session's
        // TX FIFO. No Endpoint/Function Block Discovery handling here --
        // the Host driver doesn't implement UMP Stream messages yet (see
        // the include-block comment above), so this is a raw pass-through,
        // same as UUT/USB_Host_UMP_Test.
        // Drain the attached device's FIFO rather than taking one message per
        // pass: reading once per loop iteration caps USB->network throughput
        // at the loop rate no matter how fast USB delivers. Bounded so one
        // pass cannot starve the session tick and keepalives.
        {
            static constexpr unsigned kMaxUmpPerPass = 64;
            unsigned drained = 0;
            while (s_usbHostMounted && drained < kMaxUmpPerPass &&
                   tuh_ump_available(s_usbHostDaddr, s_usbHostItfNum) > 0) {
                // Check the session has room BEFORE dequeuing: a message read
                // out and then refused has nowhere to go but the bin, whereas
                // one left in the FIFO lets USB hold the device off.
                if (sessionStarted &&
                    nm2Session->state() == SessionState::Established &&
                    nm2Session->txSpaceAvailable() == 0) break;
                drained++;

                // Whole packets only. A word-oriented read can stop mid-packet,
                // and half a packet in a UMP Data Command breaks M2-124-UM 7.2
                // and desynchronises the receiver's parse from there on. See
                // networkmidi2/Ump.h.
                if (s_usbHostFramer.hasRoomFor(4)) {
                    uint32_t UMPpacket[4];
                    uint16_t umpCount = tuh_ump_read_ntoh(s_usbHostDaddr, s_usbHostItfNum,
                                                          UMPpacket, 4);
                    if (umpCount == 0 && s_usbHostFramer.pending() == 0) break;
                    if (umpCount) s_usbHostFramer.push(UMPpacket, umpCount);
                }
                const uint32_t *pkt = nullptr;
                uint8_t pw = 0;
                bool stalled = false;
                while ((pw = s_usbHostFramer.peek(&pkt)) != 0) {
                    if (sessionStarted) {
                        if (!nm2Session->sendUmp(pkt, pw)) {
                            noteUmpDropped(pw);
                            stalled = true;   // not consumed; retried next pass
                            break;
                        }
                    }
                    s_usbHostFramer.consume(pw);
                }
                if (stalled) break;
            }
        }
#else
        // USB -> Network: drain tusb_ump and forward each UMP message into
        // the NetworkMIDI2 session's TX FIFO.
        static constexpr unsigned kMaxUmpPerPass = 64;
        // Extra USB service calls allowed per pass when the FIFO runs dry.
        //
        // At full speed each USB transfer from the host is taken in only when
        // tud_task() re-arms the OUT endpoint, and it used to run once per
        // main-loop pass -- a pass that also polls the W5500 and sends
        // datagrams over SPI, ~0.3-0.6 ms. A chord, which a host sends as one
        // transfer per note, therefore trickled in one note per pass and left
        // as one datagram per note: a ten-note chord spread over ~4-6 ms on
        // the wire. Servicing USB again when the FIFO empties gathers notes
        // arriving back to back into the same pass, and the same datagram.
        static constexpr unsigned kMaxUsbRefills = 8;
        unsigned refills = 0;
        unsigned drained = 0;
        while (tud_ump_n_mounted(0) && drained < kMaxUmpPerPass) {
            if (!tud_ump_n_available(0)) {
                if (refills >= kMaxUsbRefills) break;
                refills++;
                tud_task();
                if (!tud_ump_n_available(0)) break;
            }
            // Stream messages (MT 0xF) are answered locally and never
            // enqueued, so they must still be drained even when the session
            // has no room -- only stop when there is data we could not place.
            // Must test Established explicitly: txSpaceAvailable() reports 0
            // for any non-Established state, so keying off sessionStarted
            // alone stalls the drain for the whole PendingInvitation window --
            // and that is exactly when the Stream-message discovery handshake
            // needs to be pumped.
            if (sessionStarted &&
                nm2Session->state() == SessionState::Established &&
                nm2Session->txSpaceAvailable() == 0 &&
                tud_ump_n_available(0) > 0) {
                break;
            }
            drained++;
            if (s_usbDevFramer.hasRoomFor(4)) {
                uint32_t UMPpacket[4];
                uint8_t umpCount = tud_ump_read_ntoh(0, UMPpacket, 4);
                if (umpCount) s_usbDevFramer.push(UMPpacket, umpCount);
            }

            // Whole packets only -- see networkmidi2/Ump.h. The message type is
            // also judged per packet now: reading it from word 0 of the whole
            // read misjudged every packet after the first when one read spanned
            // several messages.
            const uint32_t *pkt = nullptr;
            uint8_t pw = 0;
            bool stalled = false;
            while ((pw = s_usbDevFramer.peek(&pkt)) != 0) {
                for (uint8_t i = 0; i < pw; i++) {
                    // Endpoint/Function Block Discovery Stream messages are
                    // handled here; everything else (Channel Voice, etc.)
                    // passes straight through to the network session.
                    UMPHandler.processUMP(pkt[i]);
                }

                // UMP Stream messages (Message Type 0xF -- Endpoint/Function
                // Block Discovery and their replies) are answered locally
                // above via midiendpoint()/functionblock(); they are USB<->
                // host session-management traffic, not MIDI data, and must
                // not also be relayed onto the NetworkMIDI2 session.
                uint8_t messageType = (pkt[0] >> 28) & 0xF;
                if (messageType != 0xF && sessionStarted) {
                    if (!nm2Session->sendUmp(pkt, pw)) {
                        noteUmpDropped(pw);
                        stalled = true;
                        break;
                    }
                }
                s_usbDevFramer.consume(pw);
            }
            if (stalled) break;
        }
#endif

        // Network -> USB happens inside tick() via onNetworkUmp() above.
        //
        // In slices of kRxDatagramsPerSlice, with the W5500 emptied into the
        // transport's RAM ring between them. macOS and Windows peers send one
        // new command per datagram, and over Wi-Fi they arrive in bursts
        // (captured: 3,000-5,400 datagrams/s for ~100 ms). One unbounded tick
        // over a burst kept the loop away from the W5500 past the ~20-30 ms
        // its 16 KB covers; it dropped frames in hardware, the session saw
        // sequence gaps no macOS/Windows sender could retransmit, and reset --
        // network -> USB delivery from those peers was 20-60%.
        //
        // USB is serviced between slices too. The USB IN endpoint is re-armed
        // only from tud_task(), and a pass over a full backlog is long; with
        // one tud_task() per pass the 16 KB USB TX FIFO drained slower than
        // Windows fills it (1 KB SysEx at 60/s: FIFO full, 60% delivered,
        // with nothing lost on the network side).
        if (sessionStarted) {
            for (unsigned slice = 0; slice < kMaxRxSlicesPerPass; ++slice) {
                nm2Session->tick();
                wiznet_lwip_poll();
                if (!nm2Session->rxBacklog()) break;
#if !NM2_BRIDGE_USB_HOST
                tud_task();
#endif
            }
        }
        }

        // Only reachable via the ESC handler above (ordinary run loop above
        // is infinite otherwise) -- loop back and re-run setup immediately,
        // with no boot-countdown gate, since pressing ESC already is the
        // user asking for it.
        runSetupNow(gBridgeConfig);
        enteredSetup = true;
    }
}
