//
// NetworkMIDI2 <-> USB MIDI 2.0 bridge for Pico 2 W (RP2350 + cyw43 WiFi).
//
// Two USB roles, selected at build time via NM2_BRIDGE_USB_ROLE (see
// CMakeLists.txt) -- same split and rationale as examples/midi_bridge/pico:
//   DEVICE (default) -- the bridge presents as a USB MIDI 2.0 device to a
//     host computer/DAW, answering Endpoint/Function Block Discovery
//     locally. Composite MIDI 2.0 (UMP) + CDC (usb_descriptors.cpp, shared
//     as-is with examples/midi_bridge/pico -- board-agnostic, no behavior
//     change to that target). CDC gives a console on boards with no
//     Picoprobe/debug UART wired up. Pico 2 W's RP2350 USB controller is
//     Full-Speed, same as the RP2040 Pico -- the composite CDC+MIDI crash
//     investigated on the NXP FRDM-MCXN947 board was specific to that
//     board's High-Speed USB controller (see examples/midi_bridge/nxp/
//     README.md); it does not apply here.
//   HOST -- the bridge is itself the USB host, bridging to a directly- or
//     hub-attached USB MIDI device via the tusb_ump Host driver, same as
//     examples/midi_bridge/pico's HOST role (raw UMP pass-through in both
//     directions, no discovery handling -- see that file's header comment).
//     USB port is then occupied being the host port, so the console is
//     UART-only (no CDC).
// NM2_BRIDGE_USB_HOST (0 or 1, set by CMakeLists.txt from that option)
// selects between the two throughout this file.
//
// Transport is WiFi/lwIP (NO_SYS=1 polling, pico_cyw43_arch_lwip_poll).
// Role, device name, and WiFi SSID/password are runtime configuration
// (bridge_config.h/console_menu.h, own copies for this target -- adapted
// from examples/midi_bridge/pico's, not shared, so that target cannot
// regress), persisted to flash and editable via the boot-time setup menu,
// same UX pattern as examples/midi_bridge/pico.
//

#include <cstring>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"

#include "lwip/netif.h"
#include "lwip/timeouts.h"

#include "networkmidi2/NetworkMidiSession.h"
#include "networkmidi2/Ump.h"
#include "networkmidi2/Types.h"
#include "LwipUdpTransport.h"
#include "LwipMdnsDiscovery.h"

#include "bridge_config.h"
#include "console_menu.h"

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

#ifndef NM2_MDNS_NAME
#define NM2_MDNS_NAME "pico2w-nm2-bridge"
#endif

#define NM2_SESSION_PORT 5004

// How long to wait for a DHCP lease before giving up and running without a
// session. Matches the NXP bridge's kDhcpTimeoutMs.
#define NM2_DHCP_TIMEOUT_MS 15000
static constexpr uint16_t NM2_CLIENT_LOCAL_PORT = 5005;

// Pressing this key at any time while the bridge is running re-enters the
// setup menu in place -- USB (MIDI + CDC console) stays up throughout, no
// re-enumeration or power cycle needed. Same pattern/rationale as
// examples/midi_bridge/pico's kReconfigureKey.
constexpr int kReconfigureKey = 0x1B; // ESC

// Minimal UMP Endpoint Discovery identity -- see examples/midi_bridge/pico's
// main.cpp for the source of these values (AmeNote has no registered SysEx
// manufacturer ID; 0x7D is the reserved "educational/non-commercial" prefix).
#define DEVICE_MFRID 0x7D, 0x00, 0x00
#define DEVICE_FAMID 0x00, 0x00
#define DEVICE_MODELID 0x00, 0x00
#define DEVICE_VERSIONID 0, 1, 0, 0

static BridgeConfig       gBridgeConfig;
static LwipUdpTransport   nm2Transport;
static NetworkMidiSession *nm2Session = nullptr;

#if NM2_BRIDGE_USB_HOST
// ---------------------------------------------------------------------------
// HOST role: track the single currently-mounted USB MIDI device. Identical
// shape to examples/midi_bridge/pico's HOST role -- see that file's comment
// on why this assumes one downstream device at a time.
// ---------------------------------------------------------------------------
static bool    s_usbHostMounted = false;
static uint8_t s_usbHostDaddr   = 0;
static uint8_t s_usbHostItfNum  = 0;

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

#include "hardware/structs/usb.h"

static umpProcessor UMPHandler;

extern "C" void tud_mount_cb(void) { printf("[USB] tud_mount_cb -- configured\n"); }
extern "C" void tud_umount_cb(void) { printf("[USB] tud_umount_cb -- unconfigured\n"); }

// 'u' on the console while running: dump the USB device controller's state.
// Bench diagnostic for the RP2350 enumeration failure (see CMakeLists.txt's
// known-issue comment). The RP2350-only registers are the informative ones:
// SM_STATE says whether the device FSM is wedged, EP_RX/TX_ERROR what the
// SIE has been seeing, DEV_ADDR_CTRL whether SET_ADDRESS was ever reached.
static void dumpUsbDeviceRegs() {
    printf("[USB] tud: inited=%d connected=%d mounted=%d suspended=%d\n",
           tud_inited(), tud_connected(), tud_mounted(), tud_suspended());
    printf("[USB] main_ctrl=%08lx sie_ctrl=%08lx sie_status=%08lx muxing=%08lx pwr=%08lx\n",
           (unsigned long) usb_hw->main_ctrl, (unsigned long) usb_hw->sie_ctrl,
           (unsigned long) usb_hw->sie_status, (unsigned long) usb_hw->muxing,
           (unsigned long) usb_hw->pwr);
    printf("[USB] dev_addr_ctrl=%08lx buf_status=%08lx inte=%08lx ints=%08lx sof=%lu\n",
           (unsigned long) usb_hw->dev_addr_ctrl, (unsigned long) usb_hw->buf_status,
           (unsigned long) usb_hw->inte, (unsigned long) usb_hw->ints,
           (unsigned long) (usb_hw->sof_rd & 0x7FF));
#if PICO_RP2350
    printf("[USB] sm_state=%08lx ep_tx_error=%08lx ep_rx_error=%08lx linestate_tuning=%08lx dev_sm_watchdog=%08lx\n",
           (unsigned long) usb_hw->sm_state, (unsigned long) usb_hw->ep_tx_error,
           (unsigned long) usb_hw->ep_rx_error, (unsigned long) usb_hw->linestate_tuning,
           (unsigned long) usb_hw->dev_sm_watchdog);
#endif
    printf("[USB] ep0 buf_ctrl in=%08lx out=%08lx\n",
           (unsigned long) usb_dpram->ep_buf_ctrl[0].in, (unsigned long) usb_dpram->ep_buf_ctrl[0].out);
}

// ---------------------------------------------------------------------------
// USB MIDI 2.0 Endpoint / Function Block Discovery replies (DEVICE role) --
// identical shape to examples/midi_bridge/pico's DEVICE role.
// ---------------------------------------------------------------------------

void tud_ump_set_itf_cb(uint8_t itf, uint8_t alt) {
    printf("[%llu] ALT_SET itf=%d alt=%d (mVersion=%d)\n",
           time_us_64(), itf, alt, alt + 1);
}

static void midiendpoint(uint8_t majVer, uint8_t minVer, uint8_t filter) {
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

static void functionblock(uint8_t fbIdx, uint8_t filter) {
    if (fbIdx != 0 && fbIdx != 0xFF) return;

    if (filter & 0x1) {
        std::array<uint32_t, 4> UMP = UMPMessage::mtFFunctionBlockInfoNotify(
                0, true, 3 /*bidirectional*/, false, false,
                0 /*firstGroup*/, 1 /*groupLength*/, 0x00, 0, 0);
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

// Called synchronously from within nm2Session->tick() for each UMP message
// received over the network -- non-blocking FIFO write, must not call
// sendUmp()/close() from here (see NetworkMidiSession's docs).
static networkmidi2::SysExDropGate s_netToUsbGate;   // see networkmidi2/Ump.h

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

        const bool writable = tuh_ump_writeable(s_usbHostDaddr, s_usbHostItfNum) >= pw;
        if (s_netToUsbGate.suppress(pkt[0])) {
            /* dropped; counted by the session */
            uint32_t term[2];
            if (s_netToUsbGate.terminatorFor(pkt[0], term) == 2 && tuh_ump_writeable(s_usbHostDaddr, s_usbHostItfNum) >= 2) {
                tuh_ump_write_hton(s_usbHostDaddr, s_usbHostItfNum, term, (uint8_t) 2);
            }
            continue;
        }
        if (!writable) {
            /* dropped; counted by the session */
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

        const bool writable = tud_ump_n_writeable(0) >= pw;
        if (s_netToUsbGate.suppress(pkt[0])) {
            /* dropped; counted by the session */
            uint32_t term[2];
            if (s_netToUsbGate.terminatorFor(pkt[0], term) == 2 && tud_ump_n_writeable(0) >= 2) {
                tud_ump_write_hton(0, term, (uint8_t) 2);
            }
            continue;
        }
        if (!writable) {
            /* dropped; counted by the session */
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
// flood the UART/USB console under sustained congestion (the exact condition
// being reported), adding CPU-time logging overhead on top of the drops.
static uint32_t s_umpDroppedWords  = 0;
static uint32_t s_lastDropReportMs = 0;

// Carries an incomplete UMP packet between reads -- twelve bytes, not a SysEx
// buffer. See networkmidi2/Ump.h.
static networkmidi2::UmpFramer s_usbDevFramer;
#if NM2_BRIDGE_USB_HOST
static networkmidi2::UmpFramer s_usbHostFramer;
#endif

static void noteUmpDropped(uint16_t wordCount) {
    s_umpDroppedWords += wordCount;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - s_lastDropReportMs >= 1000) {
        s_lastDropReportMs = now;
        printf("[%llu] NM2 sendUmp: %lu word(s) dropped in the last ~1s\n",
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
}

// Pumps the USB stack -- unconditional, every call, regardless of what else
// is going on. This is the same tight-loop pattern examples/midi_bridge/pico
// already uses for tud_task()/tuh_task() (that target disables
// pico_stdio_usb's IRQ-background task and pumps manually for exactly this
// reason). Forward-declared here so wifi_connect() below can keep USB
// serviced during its own (bounded, non-blocking-per-iteration) wait --
// previously wifi_connect() used the blocking cyw43_arch_wifi_connect_timeout_ms()
// helper and USB enumeration relied entirely on pico_stdio_usb's low-priority
// background IRQ to keep running underneath it. That introduced an untested
// combination for this codebase (IRQ-driven tud_task() servicing a composite
// MIDI+CDC device) at the same time as the move to this RP2350 board, on top
// of the actual WiFi change -- two variables changed together for no reason
// tied to WiFi itself. Pumping tud_task() explicitly here removes that
// confound and matches every other target in this project.
static void usb_task_tick();

// ---------------------------------------------------------------------------
// WiFi bring-up -- non-blocking connect with retry, using the runtime-
// configured SSID/password (bridge_config.h) instead of build-time
// NM2_WIFI_SSID/NM2_WIFI_PASSWORD cache vars. Uses cyw43_arch_wifi_connect_
// async() + a locally-pumped poll loop (same shape as the SDK's own blocking
// cyw43_arch_wifi_connect_until(), see cyw43_arch.c) instead of the blocking
// cyw43_arch_wifi_connect_timeout_ms() wrapper, specifically so usb_task_tick()
// keeps running throughout the connect/retry window.
// ---------------------------------------------------------------------------
// Hold the CYW43 powered off (WL_REG_ON low) for kCyw43PowerOffMs, keeping
// USB serviced meanwhile. cyw43_arch_init() drops WL_REG_ON for only 20 ms,
// which does not clear a chip wedged under load: after a warm reset it came
// back unresponsive (every ioctl timing out in cyw43_do_ioctl) until the board
// was unplugged. Held low for 3 s over SWD, the same wedged chip recovered at
// once; 2 s here, before every init.
static constexpr uint32_t kCyw43PowerOffMs = 2000;

// Hardware watchdog period: longer than any healthy wait between
// usb_task_tick() calls, short enough to restore a stalled bridge quickly.
static constexpr uint32_t kWatchdogMs = 8000;

static void cyw43_power_off_hold() {
    gpio_init(CYW43_DEFAULT_PIN_WL_REG_ON);
    gpio_set_dir(CYW43_DEFAULT_PIN_WL_REG_ON, GPIO_OUT);
    gpio_put(CYW43_DEFAULT_PIN_WL_REG_ON, 0);
    const absolute_time_t until = make_timeout_time_ms(kCyw43PowerOffMs);
    while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
        usb_task_tick();
        sleep_ms(1);
    }
}

// True while the CYW43 driver is initialised: wifi_connect() leaves it
// deinitialised when it fails, and nothing may poll it then.
static bool gCyw43Up = false;

static bool wifi_connect(const BridgeConfig &cfg) {
    cyw43_power_off_hold();
    if (cyw43_arch_init() != 0) {
        printf("[err] CYW43 init failed\n");
        return false;
    }
    cyw43_arch_enable_sta_mode();

    printf("Connecting to WiFi SSID \"%s\"...\n", cfg.wifiSsid);
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (attempt > 1) printf("  Retry %d/3...\n", attempt);

        int err = cyw43_arch_wifi_connect_async(cfg.wifiSsid, cfg.wifiPassword, CYW43_AUTH_WPA2_AES_PSK);
        if (err) {
            printf("[err] WiFi connect_async failed (code %d)\n", err);
            continue;
        }

        absolute_time_t deadline = make_timeout_time_ms(15000);
        int status = CYW43_LINK_UP + 1;
        while (status >= 0 && status != CYW43_LINK_UP &&
               absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
            usb_task_tick();
            cyw43_arch_poll();
            int newStatus = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (newStatus == CYW43_LINK_NONET) {
                // No matching SSID seen yet -- keep trying within this same attempt.
                err = cyw43_arch_wifi_connect_async(cfg.wifiSsid, cfg.wifiPassword, CYW43_AUTH_WPA2_AES_PSK);
                if (err) break;
                newStatus = CYW43_LINK_JOIN;
            }
            status = newStatus;
        }

        if (status == CYW43_LINK_UP) {
            printf("WiFi connected. IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
#if !NM2_PICO_W_POWERSAVE
            // Radio stays awake: no doze between beacons (NM2_PICO_W_POWERSAVE=OFF).
            printf("WiFi power save off: %d\n", cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM));
#endif
            gCyw43Up = true;
            return true;
        }
    }
    printf("[err] WiFi connect failed after 3 attempts\n");
    cyw43_arch_deinit();
    gCyw43Up = false;
    return false;
}

// ---------------------------------------------------------------------------
// WiFi link supervision. wifi_connect() only runs at boot, and nothing used to
// look at the link again: when the station lost its association (measured:
// ~12 s into 800 SysEx/s from USB, wifi_join_state fell from 0x0e01 to
// 0x0001), the bridge carried on with no network at all -- no ping, nothing
// received, its session send FIFO full, so it stopped taking USB data and the
// host's MIDI writes failed -- until someone reset it.
//
// Polled from the run loop: on loss, log what the driver reports and rejoin,
// retrying every kWifiRejoinRetryMs; the caller restarts the session once the
// link is back (returns true exactly then).
// ---------------------------------------------------------------------------
static constexpr uint32_t kWifiCheckMs       = 250;
static constexpr uint32_t kWifiRejoinRetryMs = 10000;

static bool wifi_supervise(const BridgeConfig &cfg) {
    static bool     up          = true;     // wifi_connect() just succeeded
    static uint32_t lastCheckMs = 0;
    static uint32_t lostAtMs    = 0;
    static uint32_t lastTryMs   = 0;
    static uint32_t drops       = 0;

    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - lastCheckMs < kWifiCheckMs) return false;
    lastCheckMs = now;

    // The driver does not always know it was dropped: it acts on a
    // deauthentication only for a wrong password, so after the access point
    // dropped the station under load the link still read UP -- associated,
    // bus healthy, transmit credits in hand -- while nothing arrived. A
    // session timeout (the peer stopped answering keepalives) is the evidence
    // we do get; treat it, with the link "up", as a lost link.
    static uint32_t seenTimeouts = 0;
    bool sessionTimedOut = false;
    if (nm2Session) {
        const uint32_t t = nm2Session->diagnostics().timeouts;
        sessionTimedOut = (t != seenTimeouts);
        seenTimeouts = t;
    }

    // Power the chip off and on and join again (wifi_connect() does the
    // power-off hold). Blocks for the join, pumping USB; lwIP and the bridge's
    // UDP socket survive, as lwip_init() runs only once.
    auto restartChip = [&](const char *why) {
        printf("[%llu] WiFi: %s -- restarting the CYW43\n", time_us_64(), why);
        if (gCyw43Up) cyw43_arch_deinit();
        gCyw43Up = false;
        const bool ok = wifi_connect(cfg);
        lastTryMs = to_ms_since_boot(get_absolute_time());
        if (ok) {
            up = true;
            printf("[%llu] WiFi link back after %lu ms (chip restart)\n", time_us_64(),
                   (unsigned long) (lastTryMs - lostAtMs));
        }
        return ok;
    };

    // A restart whose join failed left the driver down: retry it on the
    // interval, and touch nothing else of the driver meanwhile.
    if (!gCyw43Up) {
        if (now - lastTryMs < kWifiRejoinRetryMs) return false;
        return restartChip("still no WiFi");
    }

    const int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (up) {
        if (status == CYW43_LINK_UP && !sessionTimedOut) return false;
        up = false;
        lostAtMs = lastTryMs = now;
        ++drops;
        printf("[%llu] WiFi link lost (#%lu): %s, status %d, join state 0x%04x\n",
               time_us_64(), (unsigned long) drops,
               status == CYW43_LINK_UP ? "session timed out" : "driver link down",
               status, (unsigned) cyw43_state.wifi_join_state);
        // A session timeout with the link still "up" is the wedged-chip
        // signature (no transmit credits, ioctls timing out): only a power
        // cycle clears that. A plain link drop gets a quick rejoin first.
        if (status == CYW43_LINK_UP) return restartChip("session timed out on an 'up' link");
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_wifi_connect_async(cfg.wifiSsid, cfg.wifiPassword, CYW43_AUTH_WPA2_AES_PSK);
        return false;
    }
    // UP straight after a forced leave is the old link not yet torn down.
    if (status == CYW43_LINK_UP && now - lostAtMs >= 2000) {
        up = true;
        printf("[%llu] WiFi link back after %lu ms, IP %s\n", time_us_64(),
               (unsigned long) (now - lostAtMs), ip4addr_ntoa(netif_ip4_addr(netif_default)));
        return true;
    }
    // No link for a whole retry interval: restart the chip, whatever the
    // status says. After a drop under load the driver was seen sitting at
    // CYW43_LINK_JOIN (join state still "active", netif link down) for good,
    // so "joining" cannot be taken as progress, and a leave/join did not
    // bring it back.
    if (now - lastTryMs >= kWifiRejoinRetryMs) {
        char why[64];
        snprintf(why, sizeof why, "no link after %lu ms (status %d)",
                 (unsigned long) (now - lostAtMs), status);
        return restartChip(why);
    }
    return false;
}

// Passed to runClientHostSelect(): keeps (once WiFi is up) cyw43_arch/lwIP
// timers serviced while the boot-time host-select menu waits for input.
static bool gWifiPollReady = false;
static void net_pump() {
    if (gWifiPollReady) {
        cyw43_arch_poll();
        sys_check_timeouts();
    }
}

// Pumps the USB stack -- tuh_task() for HOST role, tud_task() for DEVICE
// role. Unconditional, every call, same shape as examples/midi_bridge/pico.
// DEVICE role also disables pico_stdio_usb's IRQ-background task (see
// CMakeLists.txt's PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0) so this is
// the ONLY caller of tud_task() -- no second, IRQ-context caller to race.
static void usb_task_tick() {
#if NM2_BRIDGE_USB_HOST
    tuh_task();
#else
    tud_task();
#endif
    // Every loop that waits -- the run loop, WiFi connect, the power-off
    // hold, the setup menu -- passes through here, so this is the one place
    // the hardware watchdog is fed (see kWatchdogMs in main()).
    watchdog_update();
}

// Block until the netif actually holds a routable address, not merely until
// WiFi associated. `networkUp` only means association succeeded and DHCP was
// started -- the address stays 0.0.0.0 for a second or more after that.
//
// Starting a session in that window binds the transport to 0.0.0.0 and sends
// invitations from an unusable source address, and acquiring the real lease
// later does NOT repair the already-started attempt. Observed on the wired
// pico sibling against a Windows host, which answered Reply-Pending 38 times
// and then gave up with Bye(Normal) while the board sat wedged. The mDNS
// host-select path already waited via LwipMdnsDiscovery::isNetifReady, but
// the auto-connect path (host already configured, setup menu skipped)
// bypassed it entirely.
//
// Returns false on timeout, in which case the caller must not start a session.
static bool waitForNetworkAddress(uint32_t timeoutMs) {
    absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
    bool announced = false;
    while (!LwipMdnsDiscovery::isNetifReady()) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            printf("\n[warn] No IP address after %lus -- check WiFi/the DHCP server.\n",
                   (unsigned long) (timeoutMs / 1000));
            return false;
        }
        if (!announced) {
            printf("Waiting for DHCP lease...\n");
            announced = true;
        }
        net_pump();
        usb_task_tick();
        sleep_ms(1);
    }
    return true;
}

// Passed to setConsolePump(): keeps USB (and, once up, WiFi/lwIP) serviced
// while the boot-time menus wait for input.
static void usb_and_net_pump() {
    usb_task_tick();
    net_pump();
}

int main() {
#if !NM2_BRIDGE_USB_HOST
    // USB first, matching examples/midi_bridge/pico's DEVICE role -- the D+
    // pull-up is only asserted once tusb_init() runs. tud_task() is pumped
    // manually via usb_task_tick() (setConsolePump() below, and the main run
    // loop) -- pico_stdio_usb's own IRQ-background task is disabled for this
    // role (PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0, see CMakeLists.txt)
    // so there is exactly one caller, matching every other target here.
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

    // A debugger-issued reset (Picoprobe during bench bring-up) resets the
    // core/logic but can leave the USB pull-up state change too brief for
    // the host/hub to register as a real detach -- the host then keeps its
    // stale, previously-enumerated descriptor set instead of re-reading the
    // new one. Same fix as examples/midi_bridge/pico's main.cpp.
    tud_disconnect();
    sleep_ms(250);
    tud_connect();
#endif

    stdio_init_all();

    printf("\r\nStarting NetworkMIDI2 Bridge (Pico 2 W, USB %s role)\n",
           NM2_BRIDGE_USB_HOST ? "HOST" : "DEVICE");
    if (watchdog_caused_reboot()) {
        printf("[watchdog] the last run stalled for %lu ms and was restarted\n",
               (unsigned long) kWatchdogMs);
    }
    // Under sustained USB -> WiFi load the CYW43 can stop answering, and its
    // driver then waits inside every send and ioctl: the run loop stops, so
    // nothing in it (wifi_supervise() included) can notice. The watchdog does:
    // a stall longer than kWatchdogMs reboots, and the reboot's power-off hold
    // (cyw43_power_off_hold()) clears the chip. Paused while a debugger halts
    // the core.
    watchdog_enable(kWatchdogMs, true);

#if NM2_BRIDGE_USB_HOST
    tusb_rhport_init_t host_init = {};
    host_init.role  = TUSB_ROLE_HOST;
    host_init.speed = TUSB_SPEED_AUTO;
    tusb_init(BOARD_TUH_RHPORT, &host_init);

    // RP2350's host controller only notifies TinyUSB of a device via an
    // edge-triggered connect interrupt -- if a device was already plugged in
    // before tusb_init() ran, there's no edge to catch and it's silently
    // never enumerated. Same workaround as examples/midi_bridge/pico's HOST
    // role.
    if (hcd_port_connect_status(BOARD_TUH_RHPORT)) {
        printf("USB device already attached at boot -- forcing enumeration\n");
        hcd_event_device_attach(BOARD_TUH_RHPORT, false);
    }
#endif
    setConsolePump(usb_and_net_pump);

    loadBridgeConfig(gBridgeConfig);
    bool enteredSetup = runConfigMenu(gBridgeConfig);
    // Keep re-running setup until a non-empty SSID is configured -- there is
    // no "run without WiFi" fallback for this build (the whole point of this
    // target is the network bridge), unlike examples/midi_bridge/pico where
    // a missing W5500 still leaves USB MIDI usable on its own.
    while (gBridgeConfig.wifiSsid[0] == '\0') {
        printf("No WiFi SSID configured -- entering setup.\n");
        runSetupNow(gBridgeConfig);
        enteredSetup = true;
    }

    bool networkUp = wifi_connect(gBridgeConfig);
    gWifiPollReady = networkUp;

    static LwipMdnsDiscovery mdnsDisc;

    // EndpointInfo (name + product ID) is captured once here -- NetworkMidiSession
    // is non-copyable/non-movable and takes it by const-ref at construction,
    // so a name change from the ESC-triggered setup below cannot be applied
    // to the already-constructed `session` without recreating it. Same
    // limitation/note as examples/midi_bridge/pico's main.cpp.
    EndpointInfo info;
    info.setName(gBridgeConfig.name);
    info.setProductId("com.amenote.pico2w.nm2-bridge");

    NetworkMidiSession::Callbacks cb;
    cb.ctx           = nullptr;
    cb.onUmp         = onNetworkUmp;
    cb.onStateChange = onNetworkStateChange;

    static NetworkMidiSession session(nm2Transport, info, cb);
    nm2Session = &session;
    // Fewer, fuller datagrams under load: see NM2_PICO_W_TX_INTERVAL_MS in
    // CMakeLists.txt (one per message stalled the CYW43).
    session.setMinTxIntervalMs(NM2_PICO_W_TX_INTERVAL_MS);

    // Outer loop: normally runs exactly once. Re-entered, without a hardware
    // reset, when ESC is pressed inside the run loop below -- only the
    // host-select -> session-(re)start sequence repeats each time. USB stays
    // up across every re-entry; a WiFi credential change still needs a
    // reconnect, handled below.
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
            haveClientHost = runClientHostSelect(gBridgeConfig, mdnsDisc, net_pump,
                                                  LwipMdnsDiscovery::isNetifReady, enteredSetup);
        }

        bool sessionStarted = true;
        UdpEndpoint clientHostEp = {};    // kept to re-invite after a WiFi outage
        bool clientSession = false;

        if (!networkUp) {
            printf("No WiFi -- running with USB %s only.\n", NM2_BRIDGE_USB_HOST ? "HOST" : "MIDI device");
            sessionStarted = false;
        } else if (gBridgeConfig.role == BridgeRole::Client && !haveClientHost) {
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
            clientHostEp  = hostEp;
            clientSession = true;
        } else {
            nm2Session->beginHost(NM2_SESSION_PORT, &mdnsDisc);
        }

        printf("Bridge running. Press ESC at any time to re-enter setup.\n");

        // ------- Loop: pump USB, WiFi/lwIP, and the NM2 session -------
        while (true) {
            usb_task_tick();

            int key = getchar_timeout_us(0);
            if (key == kReconfigureKey) {
                printf("ESC pressed -- re-entering setup...\n");
                if (sessionStarted) nm2Session->close();
                break;
            }
#if !NM2_BRIDGE_USB_HOST
            if (key == 'u' || key == 'U') dumpUsbDeviceRegs();
#endif

            if (networkUp) {
                if (gCyw43Up) {
                    cyw43_arch_poll();
                    sys_check_timeouts();
                }
                // Back from a WiFi outage: a client whose session timed out
                // meanwhile invites its host again (a host just waits).
                if (wifi_supervise(gBridgeConfig) && clientSession &&
                    nm2Session->state() == SessionState::Idle) {
                    printf("Re-inviting %s:%u after the WiFi outage\n",
                           gBridgeConfig.clientHostIp, (unsigned) clientHostEp.port);
                    nm2Session->beginClient(clientHostEp, NM2_CLIENT_LOCAL_PORT);
                }
            }

#if NM2_BRIDGE_USB_HOST
            // USB -> Network: pull decoded UMP words from the attached USB
            // MIDI device and forward each into the NetworkMIDI2 session's
            // TX FIFO. No Endpoint/Function Block Discovery handling here --
            // raw pass-through, same as examples/midi_bridge/pico's HOST role.
            // Drain the device's FIFO rather than one message per pass:
            // reading once per loop iteration caps USB->network throughput at
            // the loop rate. Bounded so one pass cannot starve the session
            // tick and keepalives.
            {
                static constexpr unsigned kMaxUmpPerPass = 64;
                unsigned drained = 0;
                while (s_usbHostMounted && drained < kMaxUmpPerPass &&
                       tuh_ump_available(s_usbHostDaddr, s_usbHostItfNum) > 0) {
                    // Check for room BEFORE dequeuing: a message read out and
                    // then refused has nowhere to go but the bin, whereas one
                    // left in the FIFO lets USB hold the device off.
                    if (sessionStarted &&
                        nm2Session->state() == SessionState::Established &&
                        nm2Session->txSpaceAvailable() == 0) break;
                    drained++;

                    // Whole packets only: a word-oriented read can stop
                    // mid-packet, and half a packet in a UMP Data Command
                    // breaks M2-124-UM 7.2 and desynchronises the receiver.
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
                                stalled = true;
                                break;
                            }
                        }
                        s_usbHostFramer.consume(pw);
                    }
                    if (stalled) break;
                }
            }
#else
            // USB -> Network: drain tusb_ump and forward each UMP message.
            static constexpr unsigned kMaxUmpPerPass = 64;
            unsigned drained = 0;
            while (tud_ump_n_mounted(0) && tud_ump_n_available(0) &&
                   drained < kMaxUmpPerPass) {
                // Stream messages (MT 0xF) are answered locally and never
                // enqueued, so they must still be drained even when the
                // session has no room.
                // Must test Established explicitly: txSpaceAvailable() reports
                // 0 for any non-Established state, so keying off sessionStarted
                // alone stalls the drain for the whole PendingInvitation
                // window -- exactly when the Stream-message discovery
                // handshake needs pumping.
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
                // Whole packets only, and the message type judged per packet:
                // reading it from word 0 of the whole read misjudged every
                // packet after the first when a read spanned several messages.
                const uint32_t *pkt = nullptr;
                uint8_t pw = 0;
                bool stalled = false;
                while ((pw = s_usbDevFramer.peek(&pkt)) != 0) {
                    for (uint8_t i = 0; i < pw; i++) {
                        UMPHandler.processUMP(pkt[i]);
                    }
                    // Stream messages (MT=0xF, Endpoint/Function Block
                    // Discovery) are answered locally above; everything else
                    // forwards on.
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
            if (sessionStarted) nm2Session->tick();
        }

        // Reconfigure requested: re-run setup, then possibly reconnect WiFi
        // if the SSID/password changed.
        char prevSsid[sizeof(gBridgeConfig.wifiSsid)];
        char prevPass[sizeof(gBridgeConfig.wifiPassword)];
        strcpy(prevSsid, gBridgeConfig.wifiSsid);
        strcpy(prevPass, gBridgeConfig.wifiPassword);

        runSetupNow(gBridgeConfig);
        enteredSetup = true;

        bool wifiChanged = strcmp(prevSsid, gBridgeConfig.wifiSsid) != 0 ||
                            strcmp(prevPass, gBridgeConfig.wifiPassword) != 0;
        if (wifiChanged) {
            printf("WiFi credentials changed -- reconnecting...\n");
            gWifiPollReady = false;
            if (gCyw43Up) cyw43_arch_deinit();
            gCyw43Up = false;
            networkUp = wifi_connect(gBridgeConfig);
            gWifiPollReady = networkUp;
        }
    }
}
