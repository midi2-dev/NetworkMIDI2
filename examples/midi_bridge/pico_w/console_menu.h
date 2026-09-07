//
// NetworkMIDI2 Pico W Bridge -- interactive boot-time setup menu, driven
// over the app's console (USB CDC and/or UART, both via stdio -- see
// main.cpp). Own copy for pico_w -- adapted from examples/midi_bridge/pico's
// console_menu.h (DHCP/static-IP prompts replaced with WiFi SSID/password);
// intentionally NOT shared, so nothing here can regress that target.
//

#pragma once

#include "bridge_config.h"
#include "networkmidi2/Discovery.h"

// Callback this module runs inside every wait loop it has -- the setup
// prompt's countdown, readLine()'s wait for the next keypress, and the
// host-discovery browse window. main() passes one that services tud_task().
// Pass nullptr to disable pumping.
void setConsolePump(void (*pump)());

// Called once at boot, before WiFi is brought up. Prints the current (saved
// or default) configuration and gives the user a few seconds to press a key
// to enter setup; if nothing is pressed in time, returns false immediately
// and `cfg` is unchanged (already loaded via loadBridgeConfig()). If the
// user enters setup, walks role -> name -> WiFi SSID/password and saves the
// result to flash before returning true.
bool runConfigMenu(BridgeConfig &cfg);

// The actual role -> name -> WiFi setup walk that runConfigMenu() runs after
// its boot-time countdown. Exposed separately so ESC-while-running (main.cpp's
// kReconfigureKey handler) can jump straight into it without a countdown --
// the user just pressed a key specifically to get here. Saves the result to
// flash before returning, same as runConfigMenu().
void runSetupNow(BridgeConfig &cfg);

// Called after WiFi is up, when cfg.role is Client and either the user just
// walked the setup menu or no host has ever been configured. Browses for
// "_midi2._udp" hosts via `disc`, presents a numbered list, and lets the
// user pick one (or enter an IP manually). Updates cfg.clientHostIp/
// clientHostPort and saves to flash.
//
// Returns true if cfg now names a host to connect to.
//
// `interactive` says whether a user is known to be sitting at the console
// (i.e. runConfigMenu() returned true, so somebody pressed a key). When it
// is false this never blocks on console input: if discovery turns up
// nothing it gives up and returns false rather than prompting for an IP
// nobody is there to type.
//
// `pollNetwork` is called every ~50ms while browsing -- this app runs lwIP
// with NO_SYS=1 (no tcpip thread), so without pumping cyw43_arch and lwIP's
// timers here, no WiFi frames -- including mDNS responses -- would ever be
// processed and discovery could never succeed no matter how long the browse
// window is.
//
// `isNetworkReady` is polled before calling disc.browse(): browse() sends
// its mDNS query synchronously and without retry, so calling it before
// DHCP/WiFi has assigned an address means the one query it gets to send
// goes out with a bogus 0.0.0.0 source and is silently dropped by real
// responders -- wait (briefly, still pumping pollNetwork) for a valid
// address first.
bool runClientHostSelect(BridgeConfig &cfg, networkmidi2::IDiscovery &disc,
                          void (*pollNetwork)(), bool (*isNetworkReady)(),
                          bool interactive);
