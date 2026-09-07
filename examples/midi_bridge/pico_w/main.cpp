//
// NetworkMIDI2 <-> USB MIDI 2.0 bridge for Pico 2 W (RP2350 + cyw43 WiFi).
//
// USB DEVICE role only (the bridge presents as a USB MIDI 2.0 device to a
// host computer/DAW, answering Endpoint/Function Block Discovery locally,
// same as examples/midi_bridge/pico's DEVICE role) -- there is no USB HOST
// role for this target.
//
// Transport is WiFi/lwIP (NO_SYS=1 polling, pico_cyw43_arch_lwip_poll).
// Role, device name, and WiFi SSID/password are runtime configuration
// (bridge_config.h/console_menu.h, own copies for this target -- adapted
// from examples/midi_bridge/pico's, not shared, so that target cannot
// regress), persisted to flash and editable via the boot-time setup menu,
// same UX pattern as examples/midi_bridge/pico.
//
// USB is composite MIDI 2.0 (UMP) + CDC (usb_descriptors.cpp, shared as-is
// with examples/midi_bridge/pico -- board-agnostic, no behavior change to
// that target). CDC gives a console on boards with no Picoprobe/debug UART
// wired up. Pico 2 W's RP2350 USB controller is Full-Speed, same as the
// RP2040 Pico -- the composite CDC+MIDI crash investigated on the NXP
// FRDM-MCXN947 board was specific to that board's High-Speed USB controller
// (see examples/midi_bridge/nxp/README.md); it does not apply here.
//

#include <cstring>

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "pico/cyw43_arch.h"

#include "lwip/netif.h"
#include "lwip/timeouts.h"

#include "networkmidi2/NetworkMidiSession.h"
#include "networkmidi2/Types.h"
#include "LwipUdpTransport.h"
#include "LwipMdnsDiscovery.h"

#include "bridge_config.h"
#include "console_menu.h"

#include "tusb.h"
#include "ump_device.h"
#include "include/umpProcessor.h"
#include "include/umpMessageCreate.h"

using namespace networkmidi2;

#ifndef NM2_MDNS_NAME
#define NM2_MDNS_NAME "pico2w-nm2-bridge"
#endif

#define NM2_SESSION_PORT 5004
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
static umpProcessor        UMPHandler;

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

// Called synchronously from within nm2Session->tick() for each UMP message
// received over the network -- non-blocking FIFO write, must not call
// sendUmp()/close() from here (see NetworkMidiSession's docs).
static void onNetworkUmp(void *ctx, const uint32_t *words, size_t wordCount) {
    (void) ctx;
    tud_ump_write_hton(0, const_cast<uint32_t *>(words), (uint8_t) wordCount);
}

static void onNetworkStateChange(void *ctx, SessionState newState) {
    (void) ctx;
    static const char *kStateNames[] = {
            "Idle", "PendingInvitation", "AuthRequired",
            "Established", "PendingReset", "PendingBye",
    };
    printf("[%llu] NM2 session state -> %s\n", time_us_64(), kStateNames[(int) newState]);
}

// ---------------------------------------------------------------------------
// WiFi bring-up -- blocking connect with retry, using the runtime-configured
// SSID/password (bridge_config.h) instead of build-time NM2_WIFI_SSID/
// NM2_WIFI_PASSWORD cache vars.
// ---------------------------------------------------------------------------
static bool wifi_connect(const BridgeConfig &cfg) {
    if (cyw43_arch_init() != 0) {
        printf("[err] CYW43 init failed\n");
        return false;
    }
    cyw43_arch_enable_sta_mode();

    printf("Connecting to WiFi SSID \"%s\"...\n", cfg.wifiSsid);
    int r = -1;
    for (int attempt = 1; attempt <= 3 && r != 0; ++attempt) {
        if (attempt > 1) {
            printf("  Retry %d/3...\n", attempt);
            sleep_ms(1500);
        }
        r = cyw43_arch_wifi_connect_timeout_ms(
                cfg.wifiSsid, cfg.wifiPassword, CYW43_AUTH_WPA2_AES_PSK, 15000);
    }
    if (r != 0) {
        printf("[err] WiFi connect failed after 3 attempts (code %d)\n", r);
        cyw43_arch_deinit();
        return false;
    }
    printf("WiFi connected. IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    return true;
}

// Passed to setConsolePump()/runClientHostSelect(): keeps (once WiFi is up)
// cyw43_arch/lwIP timers serviced while the boot-time menus wait for input.
// Does NOT call tud_task() -- pico_enable_stdio_usb(...1) is on for this
// target (see CMakeLists.txt), which installs its own low-priority
// background IRQ that already owns tud_task() for the whole process
// lifetime, starting at stdio_init_all() (before WiFi connect or any menu
// runs). TinyUSB's core loop is not reentrant against IRQ-context
// preemption, so a second, manual tud_task() call from here (or from the
// main run loop below) racing that IRQ is unsafe -- same reasoning as
// examples/midi_bridge/pico's DEVICE role, which never polls tud_task()
// manually either.
static bool gWifiPollReady = false;
static void usb_and_net_pump() {
    if (gWifiPollReady) {
        cyw43_arch_poll();
        sys_check_timeouts();
    }
}

int main() {
    // USB first, matching examples/midi_bridge/pico's DEVICE role -- the D+
    // pull-up is only asserted once tusb_init() runs. tud_task() itself is
    // never called manually anywhere in this file; once stdio_init_all()
    // below runs (pico_enable_stdio_usb is on for this target), its
    // background IRQ owns tud_task() exclusively for the rest of the
    // process lifetime, including while WiFi connect or the setup menu are
    // blocking main().
    UMPHandler.setMidiEndpoint(midiendpoint);
    UMPHandler.setFunctionBlock(functionblock);
    tusb_init();

    // A debugger-issued reset (Picoprobe during bench bring-up) resets the
    // core/logic but can leave the USB pull-up state change too brief for
    // the host/hub to register as a real detach -- the host then keeps its
    // stale, previously-enumerated descriptor set instead of re-reading the
    // new one. Same fix as examples/midi_bridge/pico's main.cpp.
    tud_disconnect();
    sleep_ms(250);
    tud_connect();

    stdio_init_all();
    setConsolePump(usb_and_net_pump);

    printf("\r\nStarting NetworkMIDI2 Bridge (Pico 2 W, USB DEVICE role)\n");

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

        // Client role: pick a host now that the network is up, either because
        // the user just walked the setup menu or because no host has ever been
        // configured (first boot).
        bool haveClientHost = gBridgeConfig.clientHostIp[0] != '\0';
        if (networkUp && gBridgeConfig.role == BridgeRole::Client &&
            (enteredSetup || !haveClientHost)) {
            haveClientHost = runClientHostSelect(gBridgeConfig, mdnsDisc, usb_and_net_pump,
                                                  LwipMdnsDiscovery::isNetifReady, enteredSetup);
        }

        bool sessionStarted = true;

        if (!networkUp) {
            printf("No WiFi -- running as a USB MIDI device only.\n");
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
        } else {
            nm2Session->beginHost(NM2_SESSION_PORT, &mdnsDisc);
        }

        printf("Bridge running. Press ESC at any time to re-enter setup.\n");

        // ------- Loop: pump (if up) WiFi/lwIP + the NM2 session -------
        // tud_task() is not called here -- see usb_and_net_pump()'s comment.
        while (true) {
            if (getchar_timeout_us(0) == kReconfigureKey) {
                printf("ESC pressed -- re-entering setup...\n");
                if (sessionStarted) nm2Session->close();
                break;
            }

            if (networkUp) {
                cyw43_arch_poll();
                sys_check_timeouts();
            }

            // USB -> Network: drain tusb_ump and forward each UMP message.
            if (tud_ump_n_mounted(0) && tud_ump_n_available(0)) {
                uint32_t UMPpacket[4];
                uint8_t umpCount = tud_ump_read_ntoh(0, UMPpacket, 4);
                if (umpCount) {
                    for (uint8_t i = 0; i < umpCount; i++) {
                        UMPHandler.processUMP(UMPpacket[i]);
                    }
                    // Stream messages (MT=0xF, Endpoint/Function Block
                    // Discovery) are answered locally above; everything else
                    // forwards on.
                    uint8_t messageType = (UMPpacket[0] >> 28) & 0xF;
                    if (messageType != 0xF && sessionStarted) {
                        if (!nm2Session->sendUmp(UMPpacket, umpCount)) {
                            printf("[%llu] NM2 sendUmp dropped %u word(s)\n",
                                   time_us_64(), umpCount);
                        }
                    }
                }
            }

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
            if (networkUp) cyw43_arch_deinit();
            networkUp = wifi_connect(gBridgeConfig);
            gWifiPollReady = networkUp;
        }
    }
}
