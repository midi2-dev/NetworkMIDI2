//
// NetworkMIDI2 Pico W Bridge -- runtime configuration (role, name, WiFi
// credentials, network settings), persisted across reboots in the last
// flash sector. Own copy for pico_w -- see examples/midi_bridge/pico's
// bridge_config.h for the Ethernet-target original this was adapted from;
// intentionally NOT shared, so nothing here can regress that target.
// Replaces the compile-time NM2_WIFI_SSID / NM2_WIFI_PASSWORD / NM2_BRIDGE_ROLE
// CMake cache vars that main.cpp used to require a rebuild to change.
//

#pragma once

#include <cstdint>
#include <cstdio>

enum class BridgeRole : uint8_t { Client = 0, Host = 1 };

struct BridgeConfig {
    BridgeRole role            = BridgeRole::Host; // first-boot default
    char       name[64]        = "NetworkMIDIUMPBridge";

    // WiFi station credentials. Empty by default -- no baked-in default
    // (avoid accidentally shipping real credentials in a public repo/binary
    // release); the setup menu requires a non-empty SSID before saving.
    char       wifiSsid[33]     = "";
    char       wifiPassword[65] = "";

    // Client role only: the host to connect to, as chosen from the mDNS
    // discovery list or entered manually in the config menu.
    char       clientHostIp[16] = "";
    uint16_t   clientHostPort   = 5004; // Network MIDI 2.0's recommended default port
};

// Loads the saved config from flash into `cfg`. If no valid config has ever
// been saved (first boot, or a CRC/magic mismatch), `cfg` is left at the
// BridgeConfig{} defaults above.
void loadBridgeConfig(BridgeConfig &cfg);

// Persists `cfg` to flash so it survives a power cycle / reset.
void saveBridgeConfig(const BridgeConfig &cfg);

// Parses a dotted-decimal IPv4 string ("192.168.1.1") into 4 octets.
// Returns false (and leaves `out` unchanged) if `s` is not a valid address.
bool parseDottedIp(const char *s, uint8_t out[4]);
