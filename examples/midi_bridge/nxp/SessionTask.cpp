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
#include "lwip/stats.h"      // inbound drop accounting for 'h'
#include "fsl_device_registers.h"   // ENET0 drop counters
#include "ethernetif.h"      // link speed/duplex, for the 'h' diagnostic
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "lwip/autoip.h"
#include "FreeRTOS.h"
#include "task.h"

#include "networkmidi2/NetworkMidiSession.h"
#include "networkmidi2/Ump.h"
#include "networkmidi2/Types.h"
#include "NxpUdpTransport.h"
#include "NxpMdnsDiscovery.h"
#if NM2_BRIDGE_USB_HOST
#include "host/usbh.h"   // tuh_vid_pid_get()
#include "host/usbh_pvt.h"   // usbh_edpt_busy(), for the IN-armed diagnostic
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

// Per-event UMP logging (printUmp() below, on every network->USB message) is
// expensive enough on an unbuffered LPUART printf to starve this task under
// sustained traffic -- suspected contributor to dropped Note On/Off and
// truncated SysEx reported against real hardware (a prototype USB MIDI
// instrument under MT3, 2026-09; see ProtoZOA/USB_Host_UMP_Test's UMP_LOG_EVENTS, same
// rationale). Default OFF: this build only logs one-shot enumeration info
// (tuh_ump_mount_cb) and state transitions, not per-event traffic.
#ifndef NM2_LOG_UMP_EVENTS
  #define NM2_LOG_UMP_EVENTS 0
#endif

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
    CLIENT_PORT,     // client: manual port entry, following CLIENT_IP -- same
                      // "Host port [%u]:" prompt as Pico's console_menu.cpp.
                      // Missing entirely before this fix: manual entry always
                      // hardcoded kHostPort, so a host listening (or NAT/port-
                      // forwarded) on anything else was simply unreachable via
                      // the "enter an IP manually" path, with no way to say so.
    SESSION_RUN,     // session live (or idle with no host yet); ESC active
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

// Placed in SRAMX (0x0400_0000), not the main SRAM.
//
// The transport is CPU-only memory: inbound datagrams are memcpy'd out of an
// lwIP pbuf into the receive ring, and outbound ones are memcpy'd into a fresh
// PBUF_RAM pbuf. No DMA master touches any of it, so unlike the USB or ENET
// buffers it does not need to live where a peripheral can reach -- which is
// exactly why it is the right thing to move out of a main SRAM that is 92%
// full. SRAMX is otherwise entirely unused, and a boot probe confirms it is
// powered, clocked and writable before anything depends on it.
//
// This is what lets the receive ring be deep enough to absorb a burst: at
// depth 32 the board was still discarding 16% of inbound datagrams under 4 KB
// SysEx, which presents as loss on the wire rather than as a buffer that was
// too small.
static NxpUdpTransport            gTransport __attribute__((section(".bss.$SRAMX")));

// ---------------------------------------------------------------------------
// Event-driven wake-up
//
// The run loop used to end in vTaskDelay(1 ms), so it woke on a FreeRTOS timer
// that has nothing to do with USB. A frame's worth of data could sit up to a
// full tick before anything looked at it, the tick drifts against USB SOF, and
// the delay is measured from when the pass *finished* -- so a pass that ran
// long pushed the next wake-up out, and the traffic that piled up meanwhile
// went out together. Three separate sources of clumping, none of them on the
// wire.
//
// Now the loop blocks on a notification and each arrival wakes it directly:
// a completed USB IN transfer (tuh_ump_rx_cb / tud_ump_rx_cb, one per USB
// frame that carried data) and each inbound datagram (the transport's RX
// hook). The 1 ms timeout stays as the floor for everything that is genuinely
// time-based -- keepalives, retransmit timers, session timeout, the console --
// so an idle bridge behaves exactly as it did.
// ---------------------------------------------------------------------------
static TaskHandle_t gSessionTaskHandle = nullptr;
extern "C" uint32_t gDrvRxGapHist[8];      // board_init.cpp

static void sessionWakeup(void)
{
    // Both call sites are task context -- tuh_task()/tud_task() for the USB
    // callbacks, lwIP's tcpip thread for the transport -- so the plain
    // (non-FromISR) notify is the correct one.
    if (gSessionTaskHandle) xTaskNotifyGive(gSessionTaskHandle);
}

// ---------------------------------------------------------------------------
// USB IN delivery histogram  ('h' while running)
//
// A wire capture showed UMP leaving this bridge quantised to ~4 ms with one
// event per datagram, while the endpoint descriptor asks for no period at all
// (bulk, bInterval=0). Something imposes that 4 ms, and the wire cannot say
// what: a backlog draining below us (host controller / TT scheduling for this
// full-speed device on a high-speed controller) and a backlog draining above
// us (our own read loop metering) produce an identical trace.
//
// So measure at the boundary. tuh_ump_raw_rx_cb() fires once per completed IN
// transfer with that transfer's exact byte count, before any MIDI1->UMP
// translation, which answers both halves directly:
//   - transfers ~4 ms apart carrying 4 bytes  -> the pacing is below us
//   - transfers clumped, carrying many bytes  -> the pacing is ours
//
// Deliberately measured in the USB task, at the moment of delivery, so nothing
// the session task does can colour the numbers.
// ---------------------------------------------------------------------------
#if NM2_BRIDGE_USB_HOST

// Microseconds, composed from the FreeRTOS tick and the SysTick counter within
// it. The tick alone is 1 ms -- the same order as the effect being measured,
// so it would quantise the answer into the shape we are trying to explain.
// Re-read the tick around SysTick->VAL to reject a rollover landing between
// the two reads. Wraps at ~71 minutes; only differences are used, so a wrap
// costs one bad sample and nothing else.
static inline uint32_t usNow(void)
{
    uint32_t t1, val, t2;
    do {
        t1  = (uint32_t)xTaskGetTickCount();
        val = SysTick->VAL;
        t2  = (uint32_t)xTaskGetTickCount();
    } while (t1 != t2);
    const uint32_t reload = SysTick->LOAD + 1u;
    return t1 * 1000u + ((reload - val) * 1000u) / reload;   // VAL counts down
}

static const uint32_t kGapEdgeUs[] = {
    125, 250, 500, 1000, 2000, 3000, 4000, 5000, 8000, 16000, 50000, 200000
};
static const char *kGapName[] = {
    "   <125us", "125-250us", "250-500us", "500us-1ms", "  1-2ms  ", "  2-3ms  ",
    "  3-4ms  ", "  4-5ms  ", "  5-8ms  ", "  8-16ms ", " 16-50ms ", " 50-200ms",
    "  >200ms "
};
static constexpr unsigned kGapBuckets = sizeof(kGapEdgeUs)/sizeof(kGapEdgeUs[0]) + 1u;

static uint32_t gGapHist[kGapBuckets];
static uint32_t gLenHist[17];          // index = bytes/4, one USB-MIDI1 event each
static uint32_t gRxXfers, gRxBytes;
static uint32_t gGapMinUs = 0xFFFFFFFFu, gGapMaxUs;
static uint32_t gLastRxUs;
static bool     gHaveLastRx;

// IN-endpoint state on passes where the RX FIFO was empty, sampled by the
// session loop. A long gap between deliveries is either the endpoint sitting
// armed while the device NAKs (the device has nothing, or is not sending) or
// the endpoint not armed at all (ours: nothing asked the device for data). The
// gap histogram alone cannot tell those apart.
static uint8_t  s_usbHostEpIn;
static uint32_t gInIdleArmed, gInIdleNotArmed, gInHasData;
static uint32_t gInNotArmedRun, gInNotArmedRunMax;

// These are written from the USB task and read/cleared from the session task
// without a lock. Deliberate: 32-bit aligned loads and stores are atomic on
// this core, so a concurrent print can only mis-attribute a sample to the
// window either side of it -- never tear a value. A lock here would put the
// USB task's delivery path behind the session task, which is exactly the
// interference this is measuring. Diagnostics only; nothing depends on them.

extern "C" void tuh_ump_raw_rx_cb(uint8_t /*daddr*/, uint8_t /*itf_num*/,
                                  uint8_t const * /*data*/, uint16_t len)
{
    const uint32_t now = usNow();
    if (gHaveLastRx) {
        const uint32_t gap = now - gLastRxUs;       // unsigned: wrap-safe
        unsigned b = 0;
        while (b < kGapBuckets - 1u && gap >= kGapEdgeUs[b]) ++b;
        gGapHist[b]++;
        if (gap < gGapMinUs) gGapMinUs = gap;
        if (gap > gGapMaxUs) gGapMaxUs = gap;
    }
    gLastRxUs   = now;
    gHaveLastRx = true;

    gRxXfers++;
    gRxBytes += len;
    gLenHist[(len / 4u) < 17u ? (len / 4u) : 16u]++;
}

static void printUsbRxHistogram(void)
{
    printf("\r\n--- USB IN deliveries: %lu transfers, %lu bytes ---\r\n",
           (unsigned long)gRxXfers, (unsigned long)gRxBytes);
    if (gRxXfers < 2) {
        printf("  (not enough samples -- play something first)\r\n");
        return;
    }
    printf("  gap between transfers: min %lu us, max %lu us\r\n",
           (unsigned long)gGapMinUs, (unsigned long)gGapMaxUs);
    for (unsigned i = 0; i < kGapBuckets; ++i) {
        if (!gGapHist[i]) continue;
        printf("    %s %6lu\r\n", kGapName[i], (unsigned long)gGapHist[i]);
    }
    printf("  bytes per transfer (4 bytes = one USB-MIDI 1.0 event):\r\n");
    for (unsigned i = 0; i < 17; ++i) {
        if (!gLenHist[i]) continue;
        printf("    %3u bytes %6lu\r\n", i * 4u, (unsigned long)gLenHist[i]);
    }
    printf("  loop passes: RX FIFO had data %lu | empty+IN armed %lu | "
           "empty+IN NOT armed %lu (longest run %lu passes)\r\n",
           (unsigned long)gInHasData, (unsigned long)gInIdleArmed,
           (unsigned long)gInIdleNotArmed, (unsigned long)gInNotArmedRunMax);
    gInHasData = gInIdleArmed = gInIdleNotArmed = 0;
    gInNotArmedRun = gInNotArmedRunMax = 0;

    const auto &tc = gTransport.txCost();
    const uint32_t cpm = SystemCoreClock / 1000000u;   // cycles per us
    if (tc.calls) {
        printf("  net sendTo: %lu calls (%lu failed) | core lock avg %lu us max %lu us"
               " | udp_sendto avg %lu us max %lu us | total in sendTo %lu ms\r\n",
               (unsigned long)tc.calls, (unsigned long)tc.fails,
               (unsigned long)(tc.lockCyc / tc.calls / cpm), (unsigned long)(tc.lockMax / cpm),
               (unsigned long)(tc.sendCyc / tc.calls / cpm), (unsigned long)(tc.sendMax / cpm),
               (unsigned long)((uint64_t)(tc.lockCyc + tc.sendCyc) / cpm / 1000u));
    }
    gTransport.resetTxCost();
    if (netif_default) {
        const phy_speed_t  sp = ethernetif_get_link_speed(netif_default);
        const phy_duplex_t dx = ethernetif_get_link_duplex(netif_default);
        printf("  ethernet link as driven by the MAC: %s %s\r\n",
               sp == kPHY_Speed10M ? "10M" : sp == kPHY_Speed100M ? "100M" : "1000M/other",
               dx == kPHY_HalfDuplex ? "HALF duplex" : "full duplex");
    }
    // Reset, so each press reports the window since the last one rather than
    // an ever-growing average that hides what just changed.
    memset(gGapHist, 0, sizeof gGapHist);
    memset(gLenHist, 0, sizeof gLenHist);
    gRxXfers = gRxBytes = gGapMaxUs = 0;
    gGapMinUs = 0xFFFFFFFFu;
    gHaveLastRx = false;
}
#else
static void printUsbRxHistogram(void)
{
    printf("  (USB IN histogram is HOST-role only)\r\n");
}
#endif

// ---------------------------------------------------------------------------
// Inbound path accounting  ('h', both roles)
//
// A peer that sends many small datagrams (macOS and Windows both put one new
// UMP Data command per datagram) loses a share of them before the session
// ever sees one, and the loss then surfaces as gaps, retransmit requests,
// Session Resets and truncated SysEx. The wire capture says how many reached
// the board's port; these say which layer inside the board dropped the rest:
//
//   MAC RX FIFO overflow    the DMA did not drain the MAC fast enough
//   DMA missed              no free RX descriptor (the ring is full)
//   link.drop               the driver could not allocate a replacement buffer
//   mbox / INPKT err        the handoff to tcpip_thread was refused
//   pbuf pool err           lwIP had no pbuf
//   udp.drop                lwIP had no taker
//   ring drop               our transport ring was full (the session task
//                           did not keep up)
//
// The ENET counters are 11 bits and clear on read, so they are polled every
// loop pass into 32-bit totals; 'h' prints the change since the last 'h'.
// ---------------------------------------------------------------------------
struct RxPathTotals {
    uint32_t mtlOverflow, mtlMissed, dmaMissed, dmaMissedOverflowed;
    uint32_t linkRecv, linkDrop, mboxErr, inpktErr, pbufPoolErr, udpRecv, udpDrop, ringDrop;
};
static RxPathTotals gRxPath, gRxPathLast;

static void rxPathPoll(void)
{
    const uint32_t mfc = ENET0->DMA_CH[0].DMA_CHX_MISS_FRAME_CNT;   // clear on read
    gRxPath.dmaMissed += mfc & ENET_DMA_CH_DMA_CHX_MISS_FRAME_CNT_MFC_MASK;
    if (mfc & ENET_DMA_CH_DMA_CHX_MISS_FRAME_CNT_MFCO_MASK) gRxPath.dmaMissedOverflowed++;
    const uint32_t mtl = ENET0->MTL_QUEUE[0].MTL_RXQX_MISSPKT_OVRFLW_CNT;   // clear on read
    gRxPath.mtlOverflow += mtl & ENET_MTL_QUEUE_MTL_RXQX_MISSPKT_OVRFLW_CNT_OVFPKTCNT_MASK;
    gRxPath.mtlMissed   += (mtl & ENET_MTL_QUEUE_MTL_RXQX_MISSPKT_OVRFLW_CNT_MISPKTCNT_MASK)
                           >> ENET_MTL_QUEUE_MTL_RXQX_MISSPKT_OVRFLW_CNT_MISPKTCNT_SHIFT;
}

// Diagnostic: the longest session tick that delivered messages, since the
// last 'h', and what it held.
static struct { uint32_t cyc, n2u, msgs; } gLongTick = {};


static void printRxPath(void)
{
    rxPathPoll();
    RxPathTotals now = gRxPath;
    now.linkRecv    = lwip_stats.link.recv;
    now.linkDrop    = lwip_stats.link.drop;
    now.mboxErr     = lwip_stats.sys.mbox.err;
    now.inpktErr    = lwip_stats.memp[MEMP_TCPIP_MSG_INPKT]->err;
    now.pbufPoolErr = lwip_stats.memp[MEMP_PBUF_POOL]->err;
    now.udpRecv     = lwip_stats.udp.recv;
    now.udpDrop     = lwip_stats.udp.drop;
    now.ringDrop    = gTransport.rxDropped();
    const RxPathTotals &l = gRxPathLast;
    printf("  rx path since last h: frames into lwIP %lu | MAC FIFO overflow %lu"
           " | DMA no-descriptor %lu (MTL missed %lu%s) | link.drop %lu"
           " | tcpip mbox err %lu, INPKT err %lu | pbuf pool err %lu"
           " | udp recv %lu drop %lu | our ring drop %lu\r\n",
           (unsigned long)(now.linkRecv - l.linkRecv),
           (unsigned long)(now.mtlOverflow - l.mtlOverflow),
           (unsigned long)(now.dmaMissed - l.dmaMissed),
           (unsigned long)(now.mtlMissed - l.mtlMissed),
           now.dmaMissedOverflowed != l.dmaMissedOverflowed ? ", counter OVERFLOWED -- a floor" : "",
           (unsigned long)(now.linkDrop - l.linkDrop),
           (unsigned long)(now.mboxErr - l.mboxErr),
           (unsigned long)(now.inpktErr - l.inpktErr),
           (unsigned long)(now.pbufPoolErr - l.pbufPoolErr),
           (unsigned long)(now.udpRecv - l.udpRecv),
           (unsigned long)(now.udpDrop - l.udpDrop),
           (unsigned long)(now.ringDrop - l.ringDrop));
    // Per-task CPU since the last 'h'. The DWT-based counter wraps every ~61
    // min at 150 MHz; unsigned differences stay right across one wrap.
    {
        static constexpr unsigned kMaxTasks = 16;
        static TaskStatus_t  st[kMaxTasks];
        static TaskHandle_t  lastH[kMaxTasks];
        static uint32_t      lastRt[kMaxTasks];
        static uint32_t      lastTotal;
        static unsigned      lastN;
        uint32_t total = 0;
        const unsigned n = (unsigned)uxTaskGetSystemState(st, kMaxTasks, &total);
        const uint32_t span = total - lastTotal;
        printf("  cpu by task since last h:");
        for (unsigned i = 0; i < n && span; ++i) {
            uint32_t prev = 0;
            for (unsigned j = 0; j < lastN; ++j)
                if (lastH[j] == st[i].xHandle) { prev = lastRt[j]; break; }
            const uint32_t d = st[i].ulRunTimeCounter - prev;
            printf(" %s %lu%%", st[i].pcTaskName, (unsigned long)((uint64_t)d * 100u / span));
        }
        printf("\r\n");
        for (unsigned i = 0; i < n; ++i) { lastH[i] = st[i].xHandle; lastRt[i] = st[i].ulRunTimeCounter; }
        lastN = n; lastTotal = total;
    }
    {
        const uint32_t *g = gTransport.rxGapHist();
        printf("  datagram arrival gaps (lwIP -> transport): <50us %lu | 50-100 %lu | 100-250 %lu |"
               " 250-500 %lu | 0.5-1ms %lu | 1-2ms %lu | 2-5ms %lu | >=5ms %lu\r\n",
               (unsigned long)g[0], (unsigned long)g[1], (unsigned long)g[2], (unsigned long)g[3],
               (unsigned long)g[4], (unsigned long)g[5], (unsigned long)g[6], (unsigned long)g[7]);
        const uint32_t cpu = SystemCoreClock / 1000000u;
        printf("  longest session tick: %lu us, delivering %lu msgs, of which in onUmp %lu us\r\n",
               (unsigned long)(gLongTick.cyc / cpu), (unsigned long)gLongTick.msgs,
               (unsigned long)(gLongTick.n2u / cpu));
        gLongTick = {};
        gTransport.resetRxGapHist();
        const uint32_t *d = gDrvRxGapHist;
        printf("  frame arrival gaps (driver -> lwIP):     <50us %lu | 50-100 %lu | 100-250 %lu |"
               " 250-500 %lu | 0.5-1ms %lu | 1-2ms %lu | 2-5ms %lu | >=5ms %lu\r\n",
               (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2], (unsigned long)d[3],
               (unsigned long)d[4], (unsigned long)d[5], (unsigned long)d[6], (unsigned long)d[7]);
        for (int i = 0; i < 8; ++i) gDrvRxGapHist[i] = 0;

    }
    printf("  FreeRTOS heap: %u bytes free now, %u at the low-water mark (of %u)\r\n",
           (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize(),
           (unsigned)configTOTAL_HEAP_SIZE);
    gRxPathLast = now;
}

#if NM2_BRIDGE_USB_HOST
extern "C" void tuh_ump_rx_cb(uint8_t /*daddr*/, uint8_t /*itf_num*/)
{
    sessionWakeup();
}
#else
extern "C" void tud_ump_rx_cb(uint8_t /*itf*/)
{
    sessionWakeup();
}
#endif
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
static bool    s_usbHostMounted = false;
static uint8_t s_usbHostDaddr   = 0;
static uint8_t s_usbHostItfNum  = 0;

// Invoked when a UMP interface finishes enumeration (ump_host.cpp). Prints
// enough about the attached device to diagnose "enumerates but no MIDI
// data flows" reports without needing a debugger: VID/PID, which alt
// setting the driver actually landed on (0 = legacy MIDI 1.0 byte stream,
// 1 = native UMP -- a MIDI-1-only device staying on alt 0 changes how
// bytes are framed and is a likely first suspect if raw pass-through looks
// like nothing is happening), the MIDIStreaming class-spec version
// (bcdMSC), and the Group Terminal Block table ump_host.cpp parsed (or
// synthesized, for an alt-0-only device) for it.
//
// This used to also fetch and print the manufacturer/product/serial-number
// strings via tuh_descriptor_get_*_string_sync() into a separate
// TUH_EPBUF_DEF-tagged scratch buffer. Removed: reproduced 100% of the
// time with a real USB MIDI 2.0 device -- across a debugger reset AND
// a full power cycle, always hanging the whole board at the exact same
// point (the first control-transfer completion that reads into that
// buffer; nothing before it, which all used TinyUSB's own internal
// enumeration buffer, ever hung). tuh_control_xfer()'s blocking path in
// usbh.c *is* a plain reentrant busy-loop pumping tuh_task() (confirmed by
// reading it -- not a semaphore wait, so no deadlock there), but that loop
// also has no timeout at all ("TODO probably some timeout to prevent
// hanged", their comment, not ours) if the transfer's completion callback
// is simply never invoked -- which is consistent with CFG_TUH_MEM_SECTION
// being defined empty in this board's tusb_config.h, landing that scratch
// buffer in ordinary SRAM rather than whatever region the chipidea HS
// controller's DMA may require for endpoint buffers (there's an unused 4 KB
// USB_RAM region in every build's memory summary that nothing currently
// targets). Not chased further -- these strings are cosmetic; VID/PID +
// alt/bcdMSC/GTB below is what actually diagnoses a mount, and none of it
// touches this buffer or these APIs.
void tuh_ump_mount_cb(uint8_t daddr, uint8_t itf_num) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);

    uint8_t  altSetting = tuh_ump_alt_setting(daddr, itf_num);
    uint16_t bcdMsc     = tuh_ump_get_bcd_msc(daddr, itf_num);

    printf("[%lu] USB HOST: UMP device mounted daddr=%u itf_num=%u\r\n",
           (unsigned long)gTransport.nowMillis(), daddr, itf_num);
    printf("  VID=0x%04X PID=0x%04X alt=%u (%s) bcdMSC=0x%04X\r\n",
           (unsigned)vid, (unsigned)pid, (unsigned)altSetting,
           altSetting == 1 ? "native UMP" : "legacy MIDI 1.0 byte stream",
           (unsigned)bcdMsc);

    // Endpoint descriptors as the device declared them. bInterval is the
    // reason this is printed: a capture showed UMP arriving from this path
    // quantised to ~4 ms with one event per delivery, and nothing above this
    // layer can undo that. Whether the period is the device's declared polling
    // interval or something the host controller imposes is the difference
    // between "expected" and "a bug worth chasing", and the descriptor is the
    // only place that answer is written down.
    //
    // bInterval units differ by transfer type and speed, so it is decoded
    // rather than printed raw: for full-speed interrupt it is frames (1 ms
    // each) directly; for high-speed interrupt, and for isochronous at any
    // speed, it is an exponent giving 2^(bInterval-1) microframes of 125 us.
    // For bulk at full speed it is meaningless (the host schedules as
    // bandwidth allows) and for bulk at high speed it caps NAK rate rather
    // than setting a period.
    tusb_desc_endpoint_t epd;
    for (int dir = 0; dir < 2; ++dir) {
        const bool in = (dir == 0);
        const bool ok = in ? tuh_ump_get_ep_in_desc(daddr, itf_num, &epd)
                           : tuh_ump_get_ep_out_desc(daddr, itf_num, &epd);
        if (!ok) { printf("  EP %s: none\r\n", in ? "IN " : "OUT"); continue; }

        const uint8_t  xfer = epd.bmAttributes.xfer;
        static const char *kXfer[] = {"control", "isochronous", "bulk", "interrupt"};
        const unsigned mps   = (unsigned)(tu_le16toh(epd.wMaxPacketSize) & 0x07FF);
        const unsigned bIntv = (unsigned)epd.bInterval;

        char period[48];
        if (xfer == TUSB_XFER_INTERRUPT || xfer == TUSB_XFER_ISOCHRONOUS) {
            // tuh_speed_get() reports the device's own speed, which is what
            // selects the interpretation -- not the controller's.
            if (tuh_speed_get(daddr) == TUSB_SPEED_FULL && xfer == TUSB_XFER_INTERRUPT)
                snprintf(period, sizeof period, "every %u ms", bIntv ? bIntv : 1u);
            else
                snprintf(period, sizeof period, "every %u us (2^%u microframes)",
                         bIntv ? (125u << (bIntv - 1)) : 125u, bIntv ? bIntv - 1u : 0u);
        } else if (xfer == TUSB_XFER_BULK) {
            snprintf(period, sizeof period, "host-scheduled (bulk)");
        } else {
            snprintf(period, sizeof period, "n/a");
        }

        printf("  EP %s: addr=0x%02X %s mps=%u bInterval=%u -> %s\r\n",
               in ? "IN " : "OUT", (unsigned)epd.bEndpointAddress,
               kXfer[xfer & 0x3], mps, bIntv, period);
    }

    printf("  Device speed: %s\r\n",
           tuh_speed_get(daddr) == TUSB_SPEED_HIGH ? "high (480 Mb/s)" :
           tuh_speed_get(daddr) == TUSB_SPEED_FULL ? "full (12 Mb/s)" : "low (1.5 Mb/s)");

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
    s_usbHostEpIn    = tuh_ump_get_ep_in_desc(daddr, itf_num, &epd) ? epd.bEndpointAddress : 0;
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

// Holds the IP entered at CLIENT_IP while CLIENT_PORT asks for the port --
// same two-step shape as Pico's console_menu.cpp manual-entry path.
static UdpEndpoint gPendingClientEp{};

static constexpr int  kMaxFoundPeers = 8;
static DiscoveredPeer gFoundPeers[kMaxFoundPeers];
static int            gFoundCount = 0;
static uint32_t       gBrowseDeadlineMs = 0; // MDNS_RESOLVE's 4 s browse window

// True from startMdnsResolve() until finishMdnsResolve() -- distinguishes
// MDNS_RESOLVE's two sub-phases, which both run in this one AppState: still
// collecting peers vs. already showing the "Select host [1-N]:" prompt.
// Needed because the vSessionTask poll below used to gate on gFoundCount==0
// instead, which stopped draining/timing out the browse the moment the
// FIRST peer showed up -- silently wedging the client in MDNS_RESOLVE
// forever with no further prompt on the very common case of >1 discoverable
// host. Pico's console_menu.cpp doesn't need an equivalent: its browse loop
// and its selection loop are two separate function calls, never the same
// state.
static bool            gMdnsBrowsing = false;

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
#if NM2_LOG_UMP_EVENTS
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
#else
    (void) w; (void) n;
#endif
}

// ---------------------------------------------------------------------------
// Session callbacks
// ---------------------------------------------------------------------------

// Reports dropped-on-send UMP words (queue full) at most once/second, as an
// aggregate count -- printing one line per drop on an unbuffered LPUART
// would flood the console under sustained congestion (the exact condition
// being reported), adding logging overhead on top of the drops themselves.
static uint32_t s_umpDroppedWords  = 0;
static uint32_t s_lastDropReportMs = 0;

static void noteUmpDropped(uint16_t wordCount)
{
    s_umpDroppedWords += wordCount;
    uint32_t now = gTransport.nowMillis();
    if (now - s_lastDropReportMs >= 1000) {
        s_lastDropReportMs = now;
        printf("[nm2] sendUmp: %u word(s) dropped in the last ~1s (queue full)\r\n",
               (unsigned)s_umpDroppedWords);
        s_umpDroppedWords = 0;
    }
}

// ---------------------------------------------------------------------------
// Bridge traffic counters
//
// The point of these is attribution, not volume. A host-side loop test can
// prove that a message was sent and never came back, but not which leg lost
// it; these counters split the path at the bridge so "USB never delivered it"
// and "we relayed it and the network lost it" are distinguishable.
//
// Counted per message type bucket because the failure we are chasing may be
// type-specific -- a path that carries SysEx but silently drops Channel Voice
// looks identical to congestion if you only count totals.
//
// Reported once per second, and only when something changed, for the same
// reason the per-event logging was removed: on an unbuffered LPUART the
// logging otherwise becomes the bottleneck it is trying to measure.
// ---------------------------------------------------------------------------
struct BridgeCounters {
    uint32_t usbTxSysExDropped = 0; // packets suppressed as part of an
                                    // abandoned SysEx (see SysExDropGate)
    uint32_t usbRxMsgs   = 0;   // read from USB
    uint32_t usbRxWords  = 0;
    uint32_t usbRxCV     = 0;   // channel voice (MT 2 and 4)
    uint32_t usbRxSysEx  = 0;   // MT 3 and 5
    uint32_t usbRxStream = 0;   // MT 0xF, answered locally, never relayed
    uint32_t netTxMsgs   = 0;   // relayed USB -> network
    uint32_t netTxDrop   = 0;   // sendUmp() refused (queue full)
    uint32_t netTxGated  = 0;   // dropped because no Established session
    uint32_t netTxStalled = 0;  // left in the USB FIFO because the session had
                                // no room -- backpressure, NOT loss
    uint32_t netRxMsgs   = 0;   // arrived from network
    uint32_t usbTxMsgs   = 0;   // relayed network -> USB
    uint32_t usbTxFail   = 0;   // USB write refused / not mounted
    uint32_t loopIters   = 0;   // main-loop passes, to tell a throughput
                                // ceiling caused by the loop rate apart from
                                // one caused by how much each pass does
    uint32_t maxDrain    = 0;   // deepest single-pass USB drain seen; equal to
                                // the cap means the FIFO was still not empty
};
static BridgeCounters gCnt;
static BridgeCounters gCntLast;
static uint32_t       gCntReportMs = 0;

// Periodic reporting suppressed ('q' while running). The debug console is the
// NXP SDK's, built WITHOUT DEBUG_CONSOLE_TRANSFER_NON_BLOCKING, so printf
// busy-waits for every character to shift out of the LPUART: at 115200 baud
// that is 86.8 us per character, and the [cnt] line is ~160 characters -- some
// 14 ms during which this task drains no USB, runs no session tick and
// transmits nothing. Whatever arrives in that window leaves in one clump
// afterwards, which is a burst generator sitting inside a loop whose whole
// purpose is a steady 1 ms cadence. Toggle it off to measure the bridge
// rather than the instrument.
//
// Defaults to QUIET. The reporting exists to diagnose the bridge, but on this
// console it also perturbs it: at ~14 ms per [cnt] line the loop stops
// draining USB and stops transmitting for that whole time, once a second.
// Anyone who runs a build without knowing to press 'q' measures the
// instrument rather than the bridge, which has already produced wrong
// conclusions. Press 'q' to turn it on when it is actually wanted.
static bool           gQuietCounters = true;

// ---------------------------------------------------------------------------
// Network loopback ('l' while running)
//
// Echo every UMP received from the network session straight back out the same
// session, bypassing USB entirely. That isolates the network stack -- lwIP,
// the ENET driver, the transport RX ring, session parsing, FEC, retransmit and
// the TX path -- so its latency and integrity can be characterised on their
// own, with nothing from the USB side able to colour the result.
//
// Worth having permanently, not just for this investigation: it is the only
// configuration where a round-trip number means the network stack and only the
// network stack, so it is the baseline every USB-path measurement should be
// read against.
//
// Not a forwarding loop: the echo goes back to the peer that sent it, never to
// ourselves, so nothing re-enters this path.
// ---------------------------------------------------------------------------
// Carries an incomplete UMP packet between reads. A USB FIFO is read in words,
// so a read can stop mid-packet; sending that half packet would break
// M2-124-UM 7.2 and desynchronise the receiver. Twelve bytes each, not a SysEx
// buffer -- see networkmidi2/Ump.h.
static networkmidi2::UmpFramer s_usbDevFramer;

// Abandons the remainder of a SysEx whose middle could not be written to USB,
// rather than dropping one packet and leaving the host assembling a message
// that silently ends short. See networkmidi2/Ump.h.
// Where does the loop time actually go?
//
// Three hypotheses were tested against the 17% inbound datagram loss on this
// board -- a deeper receive ring, relocating it to SRAMX, and batching the USB
// writes -- and none of them moved it, while the main loop still collapses from
// 942 Hz to ~185 Hz under load. Rather than guess a fourth time, measure: the
// DWT cycle counter around each suspect, reported per second beside the loop
// rate. Cheap (one register read) and only ever read here.
struct CycleBuckets {
    uint32_t sessionTick;   // NetworkMidiSession::tick(), includes delivery
    uint32_t netToUsb;      // onUmp(): the USB write path
    uint32_t usbDrain;      // USB -> session drain pass
    uint32_t usbTask;       // tud_task()/tuh_task()
    uint32_t elapsed;       // wall cycles covered by the above
};
static CycleBuckets gCyc     = {};
static CycleBuckets gCycLast = {};

static inline void cycInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}
static inline uint32_t cycNow(void) { return DWT->CYCCNT; }

static networkmidi2::SysExDropGate s_netToUsbGate;
#if NM2_BRIDGE_USB_HOST
static networkmidi2::UmpFramer s_usbHostFramer;
#endif

static bool     gLoopback      = false;
static uint32_t gLoopbackEcho  = 0;   // echoed back to the network
static uint32_t gLoopbackDrop  = 0;   // session refused (TX FIFO full)

static void bridgeCountersTick(void)
{
    if (gQuietCounters) return;
    const uint32_t now = gTransport.nowMillis();
    if (now - gCntReportMs < 1000) return;
    gCntReportMs = now;

    // "Nothing moved" ignores loopIters, which always advances -- otherwise an
    // idle bridge would print a line every second forever, which is the
    // flooding this report exists to avoid.
    BridgeCounters probe = gCnt;
    probe.loopIters = gCntLast.loopIters;
    if (memcmp(&probe, &gCntLast, sizeof(probe)) == 0) {
        gCntLast.loopIters = gCnt.loopIters;
        return;
    }

        {
            // Percent of wall time in each suspect since the last report.
            // sessionTick includes netToUsb, so the two are nested, not
            // additive -- printed as measured rather than rearranged.
            const uint32_t wall = gCyc.elapsed - gCycLast.elapsed;
            if (wall) {
                auto pct = [wall](uint32_t a, uint32_t b) -> unsigned long {
                    return (unsigned long) (((uint64_t)(a - b) * 100u) / wall);
                };
                printf("       cpu: tick %lu%% (of which net->usb %lu%%) | "
                       "usb drain %lu%% | wall %lu ms\r\n",
                       pct(gCyc.sessionTick, gCycLast.sessionTick),
                       pct(gCyc.netToUsb,    gCycLast.netToUsb),
                       pct(gCyc.usbDrain,    gCycLast.usbDrain),
                       (unsigned long) (wall / (SystemCoreClock / 1000u)));
            }
            gCycLast = gCyc;
        }

    printf("[cnt] usb.rx %lu(cv %lu sx %lu str %lu) -> net.tx %lu drop %lu gated %lu"
           " stall %lu | net.rx %lu -> usb.tx %lu fail %lu"
           " sxdrop %lu | udp.rx %lu drop %lu | loop %lu/s maxdrain %lu\r\n",
           (unsigned long)gCnt.usbRxMsgs, (unsigned long)gCnt.usbRxCV,
           (unsigned long)gCnt.usbRxSysEx, (unsigned long)gCnt.usbRxStream,
           (unsigned long)gCnt.netTxMsgs, (unsigned long)gCnt.netTxDrop,
           (unsigned long)gCnt.netTxGated, (unsigned long)gCnt.netTxStalled,
           (unsigned long)gCnt.netRxMsgs, (unsigned long)gCnt.usbTxMsgs,
           (unsigned long)gCnt.usbTxFail, (unsigned long)gCnt.usbTxSysExDropped,
           (unsigned long)gTransport.rxPackets(),
           (unsigned long)gTransport.rxDropped(),
           (unsigned long)(gCnt.loopIters - gCntLast.loopIters),
           (unsigned long)gCnt.maxDrain);

    if (gLoopback) {
        printf("[loop] echoed %lu  refused %lu\r\n",
               (unsigned long)gLoopbackEcho, (unsigned long)gLoopbackDrop);
    }

    // Loss-recovery counters, printed only when something has actually
    // happened -- on a healthy session this line never appears, so its
    // presence is itself the signal.
    if (gSession) {
        NetworkMidiSession::Diagnostics d = gSession->diagnostics();
        if (d.retransmitReqSent || d.retransmitReqRecv || d.retransmitErrSent ||
            d.retransmitErrRecv || d.sessionResetSent  || d.sessionResetRecv  ||
            d.gapsDropped       || d.gapScanMax) {
            printf("[rec] rtx.req tx %lu rx %lu | rtx.err tx %lu rx %lu"
                   " | reset tx %lu rx %lu | gaps.lost %lu maxjump %lu\r\n",
                   (unsigned long)d.retransmitReqSent, (unsigned long)d.retransmitReqRecv,
                   (unsigned long)d.retransmitErrSent, (unsigned long)d.retransmitErrRecv,
                   (unsigned long)d.sessionResetSent,  (unsigned long)d.sessionResetRecv,
                   (unsigned long)d.gapsDropped,       (unsigned long)d.gapScanMax);
        }
        // Liveness, printed unconditionally while a session exists: the
        // interesting value here is silence, which no event can announce.
        printf("[liv] ping tx %lu rx %lu reply.rx %lu | bye tx %lu rx %lu"
               " | timeouts %lu | max.silence %lu ms of %u\r\n",
               (unsigned long)d.pingSent, (unsigned long)d.pingRecv,
               (unsigned long)d.pingReplyRecv,
               (unsigned long)d.byeSent, (unsigned long)d.byeRecv,
               (unsigned long)d.timeouts, (unsigned long)d.maxRxSilenceMs,
               (unsigned)kTimeoutMs);
    }
    gCntLast = gCnt;
}

static void countUsbRx(const uint32_t *words, uint8_t wc)
{
    gCnt.usbRxMsgs++;
    gCnt.usbRxWords += wc;
    const uint8_t mt = (words[0] >> 28) & 0xF;
    if (mt == 0x2 || mt == 0x4) gCnt.usbRxCV++;
    else if (mt == 0x3 || mt == 0x5) gCnt.usbRxSysEx++;
    else if (mt == 0xF) gCnt.usbRxStream++;
}

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

static void onStateChange(void * /*ctx*/, SessionState s)
{
    printf("[%lu] NM2 session state -> %s\r\n",
           (unsigned long)gTransport.nowMillis(), stateName(s));
    // Dump liveness on every transition. The periodic report only prints when
    // a counter moved, so a session that dies goes quiet at exactly the moment
    // worth inspecting -- which is how the first attempt at this missed the
    // transition entirely.
    if (gSession && s != SessionState::Established) {
        NetworkMidiSession::Diagnostics d = gSession->diagnostics();
        printf("       liveness: ping tx %lu rx %lu reply.rx %lu | bye tx %lu rx %lu"
               " | timeouts %lu | max.silence %lu ms of %u\r\n",
               (unsigned long)d.pingSent, (unsigned long)d.pingRecv,
               (unsigned long)d.pingReplyRecv,
               (unsigned long)d.byeSent, (unsigned long)d.byeRecv,
               (unsigned long)d.timeouts, (unsigned long)d.maxRxSilenceMs,
               (unsigned)kTimeoutMs);
        printf("       inbound: cmds %lu | umpdata %lu dup %lu bad %lu -> delivered %lu\r\n",
               (unsigned long)d.cmdsRecv, (unsigned long)d.umpDataRecv,
               (unsigned long)d.umpDupDropped, (unsigned long)d.umpMalformed,
               (unsigned long)gCnt.netRxMsgs);
        // Both of these should be zero against a conforming peer. They are
        // called out separately because they name a specific misbehaviour
        // worth reporting upstream, rather than damage to be absorbed.
        if (d.umpTruncated || d.pktTrailingBytes) {
            printf("       PROTOCOL VIOLATION: ump split across commands %lu"
                   " | undecoded trailing bytes %lu\r\n",
                   (unsigned long)d.umpTruncated,
                   (unsigned long)d.pktTrailingBytes);
        }
        printf("       datagrams: max %lu bytes (buffer %u) | clamped %lu\r\n",
               (unsigned long)gTransport.rxMaxLen(), (unsigned)kMaxPacketBytes,
               (unsigned long)gTransport.rxTruncated());

        if (gConsoleTxDropped) {
            // Printed only when it happens: the console stalled and output was
            // dropped rather than the bridge being blocked waiting for it.
            printf("       console: %lu bytes dropped (debug UART stalled)\r\n",
                   (unsigned long)gConsoleTxDropped);
        }
        if (d.byeRecv) {
            const uint8_t r = gSession->lastByeReason();
            const char *why = nm2ByeReasonText(r);
            printf("       peer Bye reason: 0x%02X (%s)\r\n", r, why);
        }
    }
    if (s == SessionState::Established) {
        gSessionEstablished = true;
    }
}

#if NM2_BRIDGE_USB_HOST
// ---------------------------------------------------------------------------
// Network -> USB egress queue (HOST role)
//
// The network delivers a SysEx in a few milliseconds; a MIDI 1.0 device drains
// it at DIN speed (31250 baud, ~3 KB/s), pushing back on USB with NAKs. The
// driver's own TX FIFO is one USB packet (CFG_TUH_UMP_TX_BUFSIZE defaults to
// the 512-byte endpoint size), so anything larger than that arriving in one
// burst had nowhere to wait and the SysEx gate cut it short: through a UM-ONE
// DIN loop, 6-7 of every 8 1 KB SysEx arrived truncated. Network MIDI 2.0 has
// no way to ask the peer to wait, so the bridge has to absorb the burst.
//
// One queue here rather than a larger driver FIFO: the driver allocates its
// FIFO per device instance (CFG_TUH_UMP = 8, for hubs), and 8 x 16 KB does not
// fit, while the bridge only ever forwards to one device. 16 KB is ~5 s of DIN
// -- a typical patch dump. A stream sustained faster than the device drains
// still overflows it; that needs flow control on the network side (see
// docs/NM2_RECEIVE_CAPACITY_PROPOSAL.md), and until then the gate still cuts
// the SysEx that does not fit, counted, rather than sending it torn.
//
// Whole packets only, in order; drained into the driver as it has room, every
// loop pass and after each session tick, which also writes in runs instead of
// one call per message.
// ---------------------------------------------------------------------------
static constexpr size_t kHostTxQWords = 4096;          // 16 KB
static uint32_t s_hostTxQ[kHostTxQWords];
static size_t   s_hostTxHead  = 0;                     // oldest word
static size_t   s_hostTxCount = 0;                     // words queued
static size_t   s_hostTxHighWater = 0;                 // since the last 'h'

static size_t hostTxRoom() { return kHostTxQWords - s_hostTxCount; }

static void hostTxPush(const uint32_t *w, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        s_hostTxQ[(s_hostTxHead + s_hostTxCount + i) % kHostTxQWords] = w[i];
    s_hostTxCount += n;
    if (s_hostTxCount > s_hostTxHighWater) s_hostTxHighWater = s_hostTxCount;
}

static void hostTxDrain()
{
    if (!s_usbHostMounted) { s_hostTxHead = s_hostTxCount = 0; return; }
    size_t room = tuh_ump_writeable(s_usbHostDaddr, s_usbHostItfNum);
    while (s_hostTxCount && room) {
        // The longest run of whole packets that is contiguous in the ring and
        // fits the driver's room.
        const size_t toEnd  = kHostTxQWords - s_hostTxHead;
        const size_t contig = (s_hostTxCount < toEnd) ? s_hostTxCount : toEnd;
        size_t take = 0;
        while (take < contig) {
            uint8_t pw = networkmidi2::umpWordCount(s_hostTxQ[s_hostTxHead + take]);
            if (pw == 0) pw = 1;                       // never stall on a bad word
            if (take + pw > contig || take + pw > room) break;
            take += pw;
        }
        uint16_t wrote;
        if (take == 0) {
            // Either the next packet does not fit yet, or it straddles the
            // ring's end: copy it out to write it whole.
            uint8_t pw = networkmidi2::umpWordCount(s_hostTxQ[s_hostTxHead]);
            if (pw == 0) pw = 1;
            if (pw > room || pw > s_hostTxCount) break;
            uint32_t tmp[4];
            for (uint8_t i = 0; i < pw; ++i) tmp[i] = s_hostTxQ[(s_hostTxHead + i) % kHostTxQWords];
            wrote = tuh_ump_write_hton(s_usbHostDaddr, s_usbHostItfNum, tmp, pw);
        } else {
            wrote = tuh_ump_write_hton(s_usbHostDaddr, s_usbHostItfNum,
                                       &s_hostTxQ[s_hostTxHead], (uint16_t) take);
        }
        if (wrote == 0) { gCnt.usbTxFail++; break; }
        gCnt.usbTxMsgs++;
        s_hostTxHead   = (s_hostTxHead + wrote) % kHostTxQWords;
        s_hostTxCount -= wrote;
        room           = (room > wrote) ? room - wrote : 0;
    }
}

static void onUmp(void * /*ctx*/, const uint32_t *words, size_t count)
{
    const uint32_t cyc0 = cycNow();
    struct CycGuard {
        uint32_t start;
        ~CycGuard() { gCyc.netToUsb += cycNow() - start; }
    } cycGuard{cyc0};

    printUmp(words, count);
    // Forward network-received UMP out to the attached USB MIDI device, if
    // one is mounted. No local Endpoint/Function Block Discovery here --
    // ump_host.cpp doesn't implement the UMP Stream-message handshake yet
    // (see its header's scope notes), so this is a raw pass-through, same
    // as examples/midi_bridge/pico/main.cpp's HOST role.
    gCnt.netRxMsgs++;
    if (gLoopback) {
        // Echo immediately, in the receive callback, so the measurement does
        // not include a wait for the next drain pass -- this is the shortest
        // path back out that the stack allows.
        if (gSession && gSession->sendUmp(words, (uint16_t)count)) gLoopbackEcho++;
        else                                                       gLoopbackDrop++;
        return;
    }
    if (!s_usbHostMounted) { gCnt.usbTxFail++; return; }
    // Queue, deciding per packet: the gate may be abandoning a SysEx that did
    // not fit earlier, and a packet is only queued whole. hostTxDrain() moves
    // the queue into the driver.
    for (size_t off = 0; off < count; ) {
        const uint8_t pw = networkmidi2::umpWordCount(words[off]);
        if (pw == 0 || off + pw > count) break;
        const uint32_t *pkt = words + off;

        if (s_netToUsbGate.suppress(pkt[0])) {
            gCnt.usbTxSysExDropped++;
            uint32_t term[2];
            if (s_netToUsbGate.terminatorFor(pkt[0], term) == 2 && hostTxRoom() >= 2)
                hostTxPush(term, 2);    // close the message short but properly ended
            off += pw;
            continue;
        }
        if (hostTxRoom() < pw) {
            gCnt.usbTxFail++;
            s_netToUsbGate.noteRefused(pkt[0]);
            off += pw;
            continue;
        }
        hostTxPush(pkt, pw);
        s_netToUsbGate.noteSent(pkt[0]);
        off += pw;
    }
}
#else
// ---------------------------------------------------------------------------
// Network -> USB staging
//
// The session hands us one message per onUmp() call, and writing each one to
// tusb_ump straight away started a USB IN transfer for the first message of
// every burst, then raced the (higher-priority) USB task for the rest: each
// completion took whatever one message had been written since. Measured on
// usbmon, a 4 KB SysEx left the board as 1003 transfers of exactly 8 bytes,
// 86-169 us apart -- 72 KB/s on a 480 Mb/s link, 80 ms for one SysEx, and
// every note queued behind it waited that long.
//
// So a session pass's output is collected here and written in one call at the
// end of the pass (usbStageFlush() after gSession->tick()), letting tusb_ump
// fill 512-byte transfers. The added delay is the length of one pass.
// ---------------------------------------------------------------------------
static uint32_t s_usbStage[512];          // words; one full kMaxPacketBytes datagram is ~250
static size_t   s_usbStageWords = 0;


static void usbStageFlush()
{
    if (s_usbStageWords == 0) return;
    const uint16_t wrote = tud_ump_write_hton(kUmpItf, s_usbStage, (uint16_t) s_usbStageWords);
    if (wrote == 0) gCnt.usbTxFail++;
    else            gCnt.usbTxMsgs++;
    s_usbStageWords = 0;
}

// Room in the USB TX FIFO once what is already staged has gone in.
static size_t usbRoomWords()
{
    const size_t w = tud_ump_n_writeable(kUmpItf);
    return (w > s_usbStageWords) ? (w - s_usbStageWords) : 0;
}

static void onUmp(void * /*ctx*/, const uint32_t *words, size_t count)
{
    const uint32_t cyc0 = cycNow();
    struct CycGuard {
        uint32_t start;
        ~CycGuard() { gCyc.netToUsb += cycNow() - start; }
    } cycGuard{cyc0};

    printUmp(words, count);
    gCnt.netRxMsgs++;
    if (gLoopback) {
        if (gSession && gSession->sendUmp(words, (uint16_t)count)) gLoopbackEcho++;
        else                                                       gLoopbackDrop++;
        return;
    }
    // Forward network-received UMP out the USB MIDI 2.0 device interface.
    if (!tud_ump_n_mounted(kUmpItf)) { gCnt.usbTxFail++; return; }
    // Write straight through. An intermediate queue drained once per main-loop
    // pass was tried here and was strictly worse: the session delivers hundreds
    // of messages within a single tick, so a 128-entry queue overflowed long
    // before the drain ran -- 6448 messages lost against 32 for writing
    // directly. tusb_ump's own TX FIFO is the right place for that buffering,
    // and it is already larger than anything added in front of it.
    //
    // Ask before writing: tud_ump_write_hton() reports only what it took, so a
    // partially accepted message would go out torn in half -- worse than
    // dropping it, because the host would reassemble the remains into
    // something that was never sent.
    // Decide per packet, write per RUN of packets.
    //
    // The decision has to be per message -- abandoning a SysEx is a
    // per-message choice -- but the write does not, and the cost here is per
    // CALL rather than per byte: every tud_ump_write_hton() claims the
    // endpoint, reads the FIFO and may submit a transfer. Writing each packet
    // separately turned one call per Command into up to 32, and this loop runs
    // at over 11 000 messages/s under load, which is what drives the main loop
    // from 942 Hz down to 187 Hz. USB itself is nowhere near its limit: bulk,
    // 512-byte packets, high speed, ~130 packets/s at the rates we see.
    //
    // So contiguous packets that are all going out are written in one call,
    // and the run is only broken where a packet is suppressed or refused.
    size_t runStart = 0;         // first word of the pending run
    size_t runWords = 0;         // words accumulated in it

    auto flushRun = [&]() {
        if (runWords == 0) return;
        if (s_usbStageWords + runWords > TU_ARRAY_SIZE(s_usbStage)) usbStageFlush();
        if (runWords > TU_ARRAY_SIZE(s_usbStage)) {
            // Larger than the stage itself: write it directly (after the
            // stage, so order is kept).
            const uint16_t wrote = tud_ump_write_hton(
                kUmpItf, const_cast<uint32_t *>(words + runStart), (uint16_t) runWords);
            if (wrote == 0) gCnt.usbTxFail++;
            else            gCnt.usbTxMsgs++;
        } else {
            memcpy(&s_usbStage[s_usbStageWords], words + runStart, runWords * sizeof(uint32_t));
            s_usbStageWords += runWords;
        }
        runWords = 0;
    };

    for (size_t off = 0; off < count; ) {
        const uint8_t pw = networkmidi2::umpWordCount(words[off]);
        if (pw == 0 || off + pw > count) break;           // malformed; stop here
        const uint32_t *pkt = words + off;

        if (s_netToUsbGate.suppress(pkt[0])) {
            flushRun();
            gCnt.usbTxSysExDropped++;
            uint32_t term[2];
            if (s_netToUsbGate.terminatorFor(pkt[0], term) == 2 &&
                usbRoomWords() >= 2) {
                // Close the message the host is still assembling: short, but
                // properly ended rather than left open. Staged output first,
                // so the terminator lands after it.
                usbStageFlush();
                tud_ump_write_hton(kUmpItf, term, 2);
            }
            off += pw;
            continue;
        }
        // Room for this packet on top of what is already pending?
        if (usbRoomWords() < runWords + pw) {
            flushRun();
            if (usbRoomWords() < pw) {
                gCnt.usbTxFail++;
                s_netToUsbGate.noteRefused(pkt[0]);
                off += pw;
                continue;
            }
        }
        if (runWords == 0) runStart = off;
        runWords += pw;
        s_netToUsbGate.noteSent(pkt[0]);
        off += pw;
    }
    flushRun();
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
#if !NM2_BRIDGE_USB_HOST
            usbStageFlush();
#endif
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
    gFoundCount   = 0;
    gMdnsBrowsing = true;
    printf("\r\nSearching for NetworkMIDI2 hosts (_midi2._udp) for %u seconds...\r\n",
           (unsigned)(kHostBrowseMs / 1000));
    printf("(Enter at any time to stop early and pick from what's found so far,\r\n"
           " or 'm' + Enter to enter a host IP manually.)\r\n");
    // browse()'s return value used to be discarded, so a search that failed
    // to even start (mdns_search_service() error, netif not actually ready
    // despite WAIT_NETWORK's own check, request-ID pool exhausted, ...) was
    // indistinguishable from one that ran the full window and genuinely
    // found nothing -- both just fell through to "No hosts found" below.
    // Surface the difference: it's the first thing to rule out when a host
    // that's confirmed reachable/advertising (checked from another machine
    // on the same LAN) still doesn't show up here.
    if (!gDisc.browse()) {
        printf("[warn] mDNS search failed to start -- falling back to manual entry.\r\n");
    }
    gBrowseDeadlineMs = gTransport.nowMillis();
    gState            = AppState::MDNS_RESOLVE;
}

// Shared by the timeout and Enter/'m' paths out of MDNS_RESOLVE -- same
// selection prompt as Pico's runClientHostSelect() tail.
static void finishMdnsResolve(bool manual)
{
    gDisc.stopBrowse();
    gMdnsBrowsing = false;

    if (!manual && gFoundCount == 0) {
        // Matches Pico's runClientHostSelect(): only block on a manual-IP
        // prompt for someone actually at the console (gEnteredSetup here is
        // this file's equivalent of Pico's `interactive`, see its own
        // comment). An unattended boot that found nothing gives up quietly
        // instead -- ESC re-enters setup later.
        if (!gEnteredSetup) {
            printf("No hosts found and no console input -- continuing without a host.\r\n"
                   "Press ESC once running to enter setup and choose one.\r\n");
            gState = AppState::SESSION_RUN;
            return;
        }
        printf("No hosts found -- enter one manually.\r\n");
        manual = true;
    }

    if (manual) {
        printf("Host IP: ");
        gState = AppState::CLIENT_IP;
        return;
    }

    printf("Select host [1-%d], or 'm' for manual entry: ", gFoundCount);
    gState      = AppState::MDNS_RESOLVE; // reuse: next line is a selection digit
    gPromptMs   = gTransport.nowMillis();
    gAutoActive = true;
}

// Time the current WAIT_NETWORK attempt started -- set by
// applyNetworkAndProceed() (and re-armed by its own DHCP-timeout retry),
// read by vSessionTask's WAIT_NETWORK block.
static uint32_t gNetworkStartMs = 0;

// Set once per WAIT_NETWORK attempt, the first time the kDhcpTimeoutMs
// timeout below fires with no DHCP lease yet -- starts RFC 3927 link-local
// self-assignment as a fallback (see NM2_NetifStartAutoIp()'s own comment)
// without abandoning DHCP, which keeps retrying in parallel and will still
// take over automatically the moment a real server answers. Reset in
// applyNetworkAndProceed() so a fresh attempt (DHCP re-selected from setup,
// or ESC-reenter) starts clean.
static bool gAutoIpStarted = false;

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
    gAutoIpStarted  = false;
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
    } else if (gEnteredSetup || !gLastHostEp.isValid()) {
        // Matches Pico's console_menu.h contract for runClientHostSelect():
        // discovery runs whenever the user just walked setup OR no host has
        // ever been configured. On this board the latter is true on every
        // single boot (no flash-backed config, see the file header comment),
        // so without this an unattended/never-configured Client would skip
        // discovery entirely and fall straight to the "not starting a
        // session" message below -- never even trying to find a host.
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
        // Reused for two sub-phases, distinguished by gMdnsBrowsing (see its
        // declaration comment): still collecting peers, vs. the post-browse
        // numbered selection that finishMdnsResolve() prints once browsing
        // has stopped.
        if (gMdnsBrowsing) {
            // Same early-exit semantics as Pico's runClientHostSelect(): any
            // Enter stops browsing now and moves straight to selection (or
            // manual-IP fallback if nothing's been found yet); 'm' + Enter
            // forces manual entry even if peers were already found. Routing
            // both through finishMdnsResolve() (instead of jumping to
            // CLIENT_IP/printing "Select host" directly, as this used to)
            // is what actually stops the browse (gDisc.stopBrowse()) --
            // skipping it left discovery running in the background forever.
            finishMdnsResolve(line[0] == 'm' || line[0] == 'M');
            break;
        }
        if (line[0] >= '1' && line[0] < '1' + gFoundCount) {
            int idx = line[0] - '1';
            doBeginClient(gFoundPeers[idx].endpoint);
            break;
        }
        // 'm' must work here too, not just while browsing. The browse window
        // expires on its own after kMdnsBrowseMs, so by the time the numbered
        // list is on screen gMdnsBrowsing is already false -- which left the
        // banner's advertised "'m' + Enter" reachable only during the few
        // seconds before the user could see what had been found. Same escape
        // as Pico's console_menu.cpp selection loop.
        if (line[0] == 'm' || line[0] == 'M') {
            printf("Host IP: ");
            gState = AppState::CLIENT_IP;
            break;
        }
        // Any other answer (including a blank Enter) re-prompts, same as
        // Pico's `while (choice < 1 || choice > hostCount)` selection loop.
        printf("Select host [1-%d], or 'm' for manual entry: ", gFoundCount);
        break;

    case AppState::CLIENT_IP: {
        if (blank) { printf("Host IP: "); break; }
        ip4_addr_t addr;
        if (ip4addr_aton(line, &addr) == 0) {
            printf("Invalid address.  Host IP: ");
            break;
        }
        gPendingClientEp.ipv4 = ntohl(addr.addr);
        printf("Host port [%u]: ", (unsigned)kHostPort);
        gState = AppState::CLIENT_PORT;
        break;
    }

    case AppState::CLIENT_PORT:
        // Blank keeps kHostPort (the default every peer listens on unless
        // reconfigured) -- same "Enter keeps the bracketed default" as every
        // other prompt here. Same wording/placement as Pico's
        // runClientHostSelect() tail ("Host port [%u]:").
        gPendingClientEp.port = blank ? kHostPort : (uint16_t) atoi(line);
        doBeginClient(gPendingClientEp);
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
        } else if (c == 'l' || c == 'L') {
            gLoopback = !gLoopback;
            gLoopbackEcho = gLoopbackDrop = 0;
            printf("[loop] network loopback %s\r\n",
                   gLoopback ? "ON -- network RX echoed straight back, USB bypassed"
                             : "OFF -- normal USB bridging");
        } else if (c == 'h' || c == 'H') {
            printUsbRxHistogram();
            printRxPath();
            // Where a loss went, when the rx path above shows none: the echo
            // (loopback mode) and the session's own recovery.
            printf("  loopback: echoed %lu, refused by session TX FIFO %lu\r\n",
                   (unsigned long)gLoopbackEcho, (unsigned long)gLoopbackDrop);
            if (gSession) {
                const NetworkMidiSession::Diagnostics d = gSession->diagnostics();
                printf("  session: rtx.req tx %lu rx %lu | rtx.err tx %lu rx %lu | reset tx %lu rx %lu"
                       " | gaps.lost %lu | dup dropped %lu\r\n",
                       (unsigned long)d.retransmitReqSent, (unsigned long)d.retransmitReqRecv,
                       (unsigned long)d.retransmitErrSent, (unsigned long)d.retransmitErrRecv,
                       (unsigned long)d.sessionResetSent,  (unsigned long)d.sessionResetRecv,
                       (unsigned long)d.gapsDropped,       (unsigned long)d.umpDupDropped);
            }
#if NM2_BRIDGE_USB_HOST
            printf("  USB egress queue: high water %u of %u words since last h, %u queued now"
                   " | SysEx cut short (queue full) %lu\r\n",
                   (unsigned) s_hostTxHighWater, (unsigned) kHostTxQWords, (unsigned) s_hostTxCount,
                   (unsigned long) gCnt.usbTxSysExDropped);
            s_hostTxHighWater = s_hostTxCount;
#endif
        } else if (c == 'c' || c == 'C') {
            // One cumulative snapshot, on demand. The periodic report ('q')
            // stalls the loop ~14 ms a second, which a latency run cannot
            // tolerate; a harness reads this between cases instead, to place
            // a lost message before or after the board's USB input.
            printf("[cnt1] usb.rx %lu cv %lu sx %lu str %lu | net.tx %lu drop %lu gated %lu"
                   " stall %lu | net.rx %lu usb.tx %lu fail %lu sxdrop %lu\r\n",
                   (unsigned long)gCnt.usbRxMsgs, (unsigned long)gCnt.usbRxCV,
                   (unsigned long)gCnt.usbRxSysEx, (unsigned long)gCnt.usbRxStream,
                   (unsigned long)gCnt.netTxMsgs, (unsigned long)gCnt.netTxDrop,
                   (unsigned long)gCnt.netTxGated, (unsigned long)gCnt.netTxStalled,
                   (unsigned long)gCnt.netRxMsgs, (unsigned long)gCnt.usbTxMsgs,
                   (unsigned long)gCnt.usbTxFail, (unsigned long)gCnt.usbTxSysExDropped);
        } else if (c == 'q' || c == 'Q') {
            gQuietCounters = !gQuietCounters;
            printf("[cnt] periodic reporting %s\r\n",
                   gQuietCounters ? "OFF -- console quiet, press q for counters"
                                  : "ON -- note this blocks the loop ~14 ms per report");
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
    cycInit();      // DWT cycle counter, for the cpu%% line in the counters

#if !NM2_BRIDGE_USB_HOST
    // Respond to the USB host's UMP Endpoint/Function Block Discovery
    // requests, same as the Pico DEVICE-role build and DIN_Bridge. No
    // equivalent in the HOST role -- see onUmp()'s comment.
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
#endif

    gSessionTaskHandle = xTaskGetCurrentTaskHandle();
    NxpUdpTransport::setRxWakeup(&sessionWakeup);

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
            bool isDhcpAddr   = dhcp_supplied_address(netif_default);
            bool isAutoIpAddr = !isDhcpAddr && autoip_supplied_address(netif_default);
            bool ready = gUseDhcp
                ? (netif_is_up(netif_default) && (isDhcpAddr || isAutoIpAddr))
                : netif_is_up(netif_default);
            bool linkUp     = netif_is_link_up(netif_default);
            ip4_addr_t myIp = *netif_ip4_addr(netif_default);
            UNLOCK_TCPIP_CORE();

            if (ready) {
                printf("IP: %s  (%s)\r\n", ip4addr_ntoa(&myIp),
                       !gUseDhcp ? "static" : isAutoIpAddr ? "AutoIP, link-local" : "DHCP");
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
                    if (!gAutoIpStarted) {
                        // Fall back to link-local (RFC 3927) without giving
                        // up on DHCP -- both run concurrently on this netif;
                        // a real DHCP server, if one ever answers, still
                        // takes over automatically. Covers a direct
                        // point-to-point cable to a peer with no DHCP server
                        // of its own (board-to-board, or board-to-PC).
                        gAutoIpStarted = true;
                        printf("\r\n[warn] DHCP timeout — check Ethernet cable.\r\n"
                               "Falling back to link-local (AutoIP)...\r\n");
                        LOCK_TCPIP_CORE();
                        NM2_NetifStartAutoIp();
                        UNLOCK_TCPIP_CORE();
                    }
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
        // Gated on gMdnsBrowsing, not gFoundCount==0 -- draining/timeout must
        // keep running for the whole window even after the first peer shows
        // up (see gMdnsBrowsing's declaration comment for what broke before).
        if (gState == AppState::MDNS_RESOLVE && gMdnsBrowsing) {
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
        // Drain the attached device's FIFO rather than taking one message per
        // pass. Reading once per 1 ms loop capped USB->network throughput at
        // the loop rate no matter how fast USB delivered -- the same ceiling
        // the DEVICE role had (about 100 messages/second under load), which
        // presents as unbounded queueing rather than as loss.
        //
        // Bounded, so one pass cannot starve the session tick, keepalives and
        // console sharing this task.
        static constexpr unsigned kMaxUmpPerPass = 64;
        unsigned drained = 0;
        const uint32_t cycDrain0 = cycNow();
        while (s_usbHostMounted && drained < kMaxUmpPerPass &&
               tuh_ump_available(s_usbHostDaddr, s_usbHostItfNum) > 0) {
            // Check the session has room BEFORE taking the message out of the
            // USB FIFO. Reading first and finding the session full leaves
            // nowhere to put the message back, so it has to be discarded;
            // leaving it in the FIFO lets USB apply real backpressure to the
            // attached device instead.
            if (gSession && gSession->state() == SessionState::Established &&
                gSession->txSpaceAvailable() == 0) {
                gCnt.netTxStalled++;
                break;
            }
            drained++;

            // Read only when the framer can take a full read on top of any
            // carried tail; otherwise drain what it already holds first.
            if (s_usbHostFramer.hasRoomFor(4)) {
                uint32_t umpBuf[4];
                uint16_t wc = tuh_ump_read_ntoh(s_usbHostDaddr, s_usbHostItfNum, umpBuf, 4);
                if (wc == 0 && s_usbHostFramer.pending() == 0) break;
                if (wc > 0) {
                    countUsbRx(umpBuf, static_cast<uint8_t>(wc));
                    s_usbHostFramer.push(umpBuf, wc);
                }
            }

            // Whole packets only. A read can end mid-packet, and half a packet
            // in a UMP Data Command violates 7.2 and desynchronises the
            // receiver's parse from that point on.
            const uint32_t *pkt = nullptr;
            uint8_t pw = 0;
            bool stalled = false;
            while ((pw = s_usbHostFramer.peek(&pkt)) != 0) {
                if (gSession && gSession->state() == SessionState::Established) {
                    if (!gSession->sendUmp(pkt, pw)) {
                        gCnt.netTxDrop++;
                        noteUmpDropped(pw);
                        stalled = true;   // not consumed; offered again next pass
                        break;
                    }
                    gCnt.netTxMsgs++;
                } else {
                    gCnt.netTxGated++;
                }
                s_usbHostFramer.consume(pw);
            }
            if (stalled) break;           // let USB hold the device off
        }
        gCyc.usbDrain += cycNow() - cycDrain0;
        if (drained > gCnt.maxDrain) gCnt.maxDrain = drained;

        if (s_usbHostMounted && s_usbHostEpIn) {
            if (tuh_ump_available(s_usbHostDaddr, s_usbHostItfNum) > 0) {
                gInHasData++;
                gInNotArmedRun = 0;
            } else if (usbh_edpt_busy(s_usbHostDaddr, s_usbHostEpIn)) {
                gInIdleArmed++;
                gInNotArmedRun = 0;
            } else {
                gInIdleNotArmed++;
                if (++gInNotArmedRun > gInNotArmedRunMax) gInNotArmedRunMax = gInNotArmedRun;
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
        // Drain the USB RX FIFO, rather than taking one message per pass.
        //
        // This loop runs on a 1 ms cadence, so reading a single UMP message
        // per iteration capped USB->network throughput at the loop rate no
        // matter how fast USB delivered -- which defeats the point of running
        // this on a high-speed part. The visible symptom was not packet loss
        // (the bridge reported zero drops) but unbounded queueing: a 6 second
        // test was still draining its backlog minutes later, with round-trip
        // latency climbing past 12 seconds. From the host's side that is
        // indistinguishable from loss, which is how it gets reported.
        //
        // Bounded rather than "while available": an unbounded drain would let
        // a fast sender starve the session tick, keepalives and the console in
        // this same task. The bound is high enough that the FIFO empties in
        // normal use and low enough that one pass cannot monopolise the loop.
        static constexpr unsigned kMaxUmpPerPass = 64;
        unsigned drained = 0;
        const uint32_t cycDrain0 = cycNow();
        while (tud_ump_n_mounted(kUmpItf) && tud_ump_n_available(kUmpItf) &&
               drained < kMaxUmpPerPass) {
            // Check the session has room BEFORE taking the message out of the
            // USB FIFO. Reading first and discovering the session is full
            // leaves nowhere to put the message back, so it has to be dropped;
            // leaving it in the USB FIFO instead lets USB NAK the host, which
            // is backpressure the sender can actually act on. Stream messages
            // (MT 0xF) are answered locally and never enqueued, so they must
            // still be drained even when the session has no room.
            if (gSession && gSession->state() == SessionState::Established &&
                gSession->txSpaceAvailable() == 0 &&
                tud_ump_n_available(kUmpItf) > 0) {
                gCnt.netTxStalled++;
                break;
            }
            drained++;
            if (s_usbDevFramer.hasRoomFor(4)) {
                uint32_t umpBuf[4];
                uint8_t  wc = tud_ump_read_ntoh(kUmpItf, umpBuf, 4);
                if (wc > 0) {
                    countUsbRx(umpBuf, wc);
                    s_usbDevFramer.push(umpBuf, wc);
                }
            }

            // Whole packets only -- see the framer's header. The message type
            // is also read per packet now: taking it from word 0 of a 4-word
            // read misjudged every packet after the first when a read spanned
            // several messages.
            const uint32_t *pkt = nullptr;
            uint8_t pw = 0;
            bool stalled = false;
            while ((pw = s_usbDevFramer.peek(&pkt)) != 0) {
                for (uint8_t i = 0; i < pw; ++i) {
                    // Endpoint/Function Block Discovery Stream messages are
                    // handled here; everything else (Channel Voice, etc.)
                    // passes straight through to the network session.
                    UMPHandler.processUMP(pkt[i]);
                }

                // UMP Stream messages (Message Type 0xF) are answered
                // locally above via midiendpoint()/functionblock(); they are
                // USB<->host session-management traffic, not MIDI data, and
                // must not also be relayed onto the NetworkMIDI2 session.
                uint8_t messageType = (pkt[0] >> 28) & 0xF;
                if (messageType != 0xF) {
                    if (gSession && gSession->state() == SessionState::Established) {
                        if (!gSession->sendUmp(pkt, pw)) {
                            gCnt.netTxDrop++;
                            noteUmpDropped(pw);
                            // Stop draining the moment the session refuses:
                            // leaving the rest in the USB FIFO lets USB apply
                            // real backpressure to the host, which is what a
                            // sender can actually respond to. Reading them out
                            // only to discard them converts a slow link into
                            // silent data loss -- measured at 92% of messages
                            // discarded here once the read loop was fast
                            // enough to keep the FIFO empty.
                            //
                            // The packet is deliberately NOT consumed, so it is
                            // offered again on the next pass rather than lost.
                            stalled = true;
                            break;
                        }
                        gCnt.netTxMsgs++;
                    } else {
                        // Counted rather than silently discarded: traffic
                        // arriving before the session is up looks exactly like
                        // loss from the host's point of view.
                        gCnt.netTxGated++;
                    }
                }
                s_usbDevFramer.consume(pw);
            }
            if (stalled) break;
        }
        gCyc.usbDrain += cycNow() - cycDrain0;
        if (drained > gCnt.maxDrain) gCnt.maxDrain = drained;
#endif // NM2_BRIDGE_USB_HOST

        gCnt.loopIters++;
        rxPathPoll();       // ENET drop counters are 11-bit, clear-on-read
        gCyc.elapsed = cycNow();     // free-running; wraps every ~29 s at 150 MHz
        bridgeCountersTick();

        // ---- Session tick ----
        if (gSession) {
            const uint32_t cycTick0 = cycNow();
            const uint32_t n2u0 = gCyc.netToUsb, msgs0 = gCnt.netRxMsgs;
            gSession->tick();
            const uint32_t tickEnd = cycNow();
#if !NM2_BRIDGE_USB_HOST
            usbStageFlush();        // one USB write for everything this pass delivered
#else
            hostTxDrain();          // as much of the egress queue as the device will take
#endif
            gCyc.sessionTick += cycNow() - cycTick0;
            if (gCnt.netRxMsgs != msgs0 && tickEnd - cycTick0 > gLongTick.cyc) {
                gLongTick.cyc   = tickEnd - cycTick0;
                gLongTick.n2u   = gCyc.netToUsb - n2u0;
                gLongTick.msgs  = gCnt.netRxMsgs - msgs0;
            }
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

        // Block until something arrives, or 1 ms passes -- whichever comes
        // first. A USB frame carrying data and an inbound datagram each wake
        // this immediately (see sessionWakeup above), so a message is packaged
        // and sent on the pass it arrived in rather than on the next timer
        // edge. pdTRUE clears the count, so a burst of notifications collapses
        // into one pass that drains everything queued -- the drain loops are
        // already bounded, so that is exactly the intended behaviour.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
}
