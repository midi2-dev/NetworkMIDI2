/**
 * @file board_init.cpp
 * @brief Board-level initialisation for FRDM-MCXN947.
 *
 * Implements NM2_BoardInit(), NM2_NetifInit(), and NM2_GetCharNonBlocking()
 * using the NXP MCUXpresso SDK 2.16.x ENET driver (Synopsys DWC Ethernet) with
 * LAN8741 PHY and lwIP raw API in FreeRTOS (NO_SYS=0, LWIP_TCPIP_CORE_LOCKING=1).
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

#include "board_init.h"

// NXP SDK board support
#include "board.h"
#include "clock_config.h"
#include "pin_mux.h"
#include "fsl_debug_console.h"

// NXP SDK MCX ENET driver (Synopsys DWC, named "ENET" not "ENET_QOS" on MCXN947)
#include "fsl_enet.h"
// LPUART driver (for NM2_GetCharNonBlocking)
#include "fsl_lpuart.h"
// LAN8741 PHY (on FRDM-MCXN947 RJ45 port J15)
#include "fsl_phy.h"
#include "fsl_phylan8741.h"

// ROM API flash driver -- for FFR_GetUUID() in deriveUniqueMac() below.
#include "fsl_flash.h"
#include "fsl_flash_ffr.h"

// lwIP
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "lwip/autoip.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/netifapi.h"
#include "lwip/dns.h"

// NXP SDK lwIP MCX Ethernet netif
extern "C" {
#include "ethernetif.h"
}

// FreeRTOS
#include "FreeRTOS.h"
#include "semphr.h"

// TinyUSB — forwards the USB1 HS controller's interrupt into TinyUSB's core.
#include "tusb.h"

#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// "Development" MAC base for FRDM-MCXN947 -- locally-administered (0x02) +
// a fixed 2-byte prefix (0x12, 0x13). The last 3 bytes are a hardcoded
// fallback, overwritten per-board by deriveUniqueMac() below with a fold of
// this chip's factory UUID whenever NM2_NetifAdd(nullptr) is used (every
// caller today -- see main.cpp). Before this fix every NXP board built from
// this firmware shared this exact fixed value, which collides as soon as a
// second one is on the same network -- unlike the Pico/ProtoZOA bridge
// (examples/midi_bridge/pico/wiznet_port/w5x00_lwip.c), which already
// derives its last 3 bytes from the RP2040's own unique flash ID.
static uint8_t kDefaultMac[6] = { 0x02, 0x12, 0x13, 0x10, 0x15, 0x11 };

// Overwrites kDefaultMac[3..5] with a fold of this chip's factory-programmed
// 128-bit UUID (FFR_GetUUID(), via the MCXN947 ROM API's Flash Firmware
// Region/CMPA page) -- XOR-folding all 16 bytes down to 3 rather than just
// truncating, same rationale as the Pico bridge's own fold: chips from the
// same manufacturing batch/wafer are known to share ID prefixes, which
// truncation would just reproduce as another collision. Leaves kDefaultMac
// at its hardcoded (still valid, just non-unique) fallback if the UUID read
// fails for any reason -- this must never be allowed to block bringing the
// netif up.
static void deriveUniqueMac(void)
{
    flash_config_t flashConfig;
    memset(&flashConfig, 0, sizeof(flashConfig));
    if (FLASH_Init(&flashConfig) != kStatus_Success) return;
    if (FFR_Init(&flashConfig) != kStatus_Success) return;

    uint8_t uuid[16];
    if (FFR_GetUUID(&flashConfig, uuid) != kStatus_Success) return;

    kDefaultMac[3] = uuid[0] ^ uuid[4] ^ uuid[8]  ^ uuid[12];
    kDefaultMac[4] = uuid[1] ^ uuid[5] ^ uuid[9]  ^ uuid[13];
    kDefaultMac[5] = uuid[2] ^ uuid[6] ^ uuid[10] ^ uuid[14];
}

// PHY address on the ENET MDIO bus (LAN8741 straps PHYAD[2:0] = 000 → addr 0)
#define NM2_PHY_ADDRESS  0U

// RMII reference clock to PHY (50 MHz)
#define NM2_ENET_CLK_HZ  50000000U

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static struct netif          s_netif;

// Diagnostic: frame arrival gaps at the driver -> lwIP handoff (the SDK's RX
// task calling netif->input), bucketed like NxpUdpTransport::rxGapHist():
// <50us, 50-100, 100-250, 250-500, 0.5-1ms, 1-2ms, 2-5ms, >=5ms. Paired with
// that histogram it says whether an inbound burst is paced in the driver
// (gaps already here) or inside lwIP's thread (gaps appear only later).
extern "C" uint32_t gDrvRxGapHist[8];
uint32_t gDrvRxGapHist[8] = {};

static uint32_t s_drvRxLastCyc = 0;
static err_t timedTcpipInput(struct pbuf *p, struct netif *inp)
{
    const uint32_t c  = DWT->CYCCNT;
    const uint32_t us = (c - s_drvRxLastCyc) / (SystemCoreClock / 1000000u);
    s_drvRxLastCyc = c;
    static const uint32_t kEdgeUs[7] = {50, 100, 250, 500, 1000, 2000, 5000};
    unsigned b = 0;
    while (b < 7 && us >= kEdgeUs[b]) ++b;
    ++gDrvRxGapHist[b];
    return tcpip_input(p, inp);
}
static phy_handle_t          s_phyHandle;
static phy_lan8741_resource_t s_phyResource;
static ethernetif_config_t   s_ethConfig;

// ---------------------------------------------------------------------------
// MDIO callbacks — used by fsl_phylan8741.c via the resource pointer
// ---------------------------------------------------------------------------

static status_t MDIO_Write(uint8_t phyAddr, uint8_t regAddr, uint16_t data)
{
    return ENET_MDIOWrite(ENET0, phyAddr, regAddr, data);
}

static status_t MDIO_Read(uint8_t phyAddr, uint8_t regAddr, uint16_t *pData)
{
    return ENET_MDIORead(ENET0, phyAddr, regAddr, pData);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void NM2_BoardInit(void)
{
    // Enable InputMux clock (needed by pin_mux.c)
    CLOCK_EnableClock(kCLOCK_InputMux);

    // Clock, pin mux, and debug console (LPUART4 → MCU-Link virtual COM port).
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitDebugConsole();

    // ENET clock: attach ENET RMII clock source (input 0 = external 50 MHz osc
    // on FRDM-MCXN947), enable clock gate, reset ENET.
    // Use the named enum to satisfy C++ strict enum-type checking.
    CLOCK_AttachClk(kNONE_to_ENETRMII);
    CLOCK_EnableClock(kCLOCK_Enet);
    SYSCON0->PRESETCTRL2 = SYSCON_PRESETCTRL2_ENET_RST_MASK;
    SYSCON0->PRESETCTRL2 &= ~SYSCON_PRESETCTRL2_ENET_RST_MASK;

    // Configure ENET SMI (MDIO) clock for PHY register access.
    ENET_SetSMI(ENET0, CLOCK_GetCoreSysClkFreq());

    // Newlib-nano defaults to fully-buffered stdout on bare-metal.
    // Switch to unbuffered so printf() flushes immediately via _write().
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Set ENET interrupt priorities to configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
    // so they are safe to call FreeRTOS FromISR APIs (CMSIS value, not shifted).
    // Default priority is 0 (highest), which violates FreeRTOS's BASEPRI mask.
    NVIC_SetPriority(ETHERNET_IRQn,      configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    NVIC_SetPriority(ETHERNET_PMT_IRQn,  configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
    NVIC_SetPriority(ETHERNET_MACLP_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
}

void NM2_NetifAdd(const uint8_t *mac6)
{
    if (!mac6) {
        deriveUniqueMac();
    }
    const uint8_t *mac = mac6 ? mac6 : kDefaultMac;

    // Wire up MDIO read/write callbacks for LAN8741 PHY driver.
    s_phyResource.read  = MDIO_Read;
    s_phyResource.write = MDIO_Write;

    // Populate the ethernetif config for ethernetif0_init().
    memset(&s_ethConfig, 0, sizeof(s_ethConfig));
    s_ethConfig.phyHandle   = &s_phyHandle;
    s_ethConfig.phyAddr     = NM2_PHY_ADDRESS;
    s_ethConfig.phyOps      = &phylan8741_ops;
    s_ethConfig.phyResource = &s_phyResource;
    s_ethConfig.srcClockHz  = NM2_ENET_CLK_HZ;
    memcpy(s_ethConfig.macAddress, mac, 6);

    // Add the ENET netif, but leave it down -- DHCP vs. static IP is a
    // runtime setup-menu choice made later, from vSessionTask (see
    // NM2_NetifStartDhcp()/NM2_NetifSetStatic() below).
    // Called from main() before vTaskStartScheduler(), so tcpip_thread has not
    // started yet — netif_add() is safe without core locking.
    printf("MAC address: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ip4_addr_t zero = IPADDR4_INIT(0U);
    netif_add(&s_netif, &zero, &zero, &zero,
              &s_ethConfig, ethernetif0_init, timedTcpipInput);
    netif_set_default(&s_netif);
}

void NM2_NetifStartDhcp(void)
{
    netif_set_up(&s_netif);
    dhcp_start(&s_netif);
}

void NM2_NetifStartAutoIp(void)
{
    autoip_start(&s_netif);
}

bool NM2_NetifSetStatic(const char *ip, const char *netmask, const char *gateway, const char *dns)
{
    ip4_addr_t ipAddr, nmAddr, gwAddr;
    if (!ip4addr_aton(ip, &ipAddr) || !ip4addr_aton(netmask, &nmAddr) || !ip4addr_aton(gateway, &gwAddr)) {
        return false;
    }

    netif_set_addr(&s_netif, &ipAddr, &nmAddr, &gwAddr);
    netif_set_up(&s_netif);

    if (dns && dns[0]) {
        ip4_addr_t dnsAddr;
        if (ip4addr_aton(dns, &dnsAddr)) {
            ip_addr_t dnsIpAddr = IPADDR4_INIT(dnsAddr.addr);
            dns_setserver(0, &dnsIpAddr);
        }
    }

    return true;
}

// FreeRTOS run-time stats clock (FreeRTOSConfig.h): the DWT cycle counter / 128.
extern "C" void nm2RunTimeStatsInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
}

extern "C" uint32_t nm2RunTimeStatsNow(void)
{
    return DWT->CYCCNT >> 7;
}

void NM2_UsbHsInit(void)
{
    // Bump DCDC to 1.8V / CORELDO to 1.1V (default is 1.8V/1.0V) -- required
    // headroom for the USB HS PHY. Verbatim from TinyUSB's hw/bsp/mcx/family.c
    // board_init() BOARD_TUD_RHPORT==1 branch (register-level only; no
    // dependency on TinyUSB's own board.c/pin_mux.c/clock_config.c).
    SPC0->ACTIVE_VDELAY = 0x0500;
    SPC0->ACTIVE_CFG &= ~SPC_ACTIVE_CFG_CORELDO_VDD_DS_MASK;
    SPC0->ACTIVE_CFG |= SPC_ACTIVE_CFG_DCDC_VDD_LVL(0x3) | SPC_ACTIVE_CFG_CORELDO_VDD_LVL(0x3) |
                        SPC_ACTIVE_CFG_SYSLDO_VDD_DS_MASK | SPC_ACTIVE_CFG_DCDC_VDD_DS(0x2u);
    while (SPC0->SC & SPC_SC_BUSY_MASK) {}

    if (0u == (SCG0->LDOCSR & SCG_LDOCSR_LDOEN_MASK)) {
        SCG0->TRIM_LOCK = 0x5a5a0001U;
        SCG0->LDOCSR |= SCG_LDOCSR_LDOEN_MASK;
        while (0U == (SCG0->LDOCSR & SCG_LDOCSR_VOUT_OK_MASK)) {}
    }

    SYSCON->AHBCLKCTRLSET[2] |= SYSCON_AHBCLKCTRL2_USB_HS_MASK | SYSCON_AHBCLKCTRL2_USB_HS_PHY_MASK;

    // System oscillator, 20-30 MHz crystal (FRDM-MCXN947's board crystal), for
    // the USB HS PHY PLL reference.
    SCG0->SOSCCFG &= ~(SCG_SOSCCFG_RANGE_MASK | SCG_SOSCCFG_EREFS_MASK);
    SCG0->SOSCCFG = (1U << SCG_SOSCCFG_RANGE_SHIFT) | (1U << SCG_SOSCCFG_EREFS_SHIFT);
    SCG0->SOSCCSR |= SCG_SOSCCSR_SOSCEN_MASK;
    while (0 == (SCG0->SOSCCSR & SCG_SOSCCSR_SOSCVLD_MASK)) {}

    SYSCON->CLOCK_CTRL |= SYSCON_CLOCK_CTRL_CLKIN_ENA_MASK | SYSCON_CLOCK_CTRL_CLKIN_ENA_FM_USBH_LPT_MASK;
    CLOCK_EnableClock(kCLOCK_UsbHs);
    CLOCK_EnableClock(kCLOCK_UsbHsPhy);
    CLOCK_EnableUsbhsPhyPllClock(kCLOCK_Usbphy480M, 24000000U);
    CLOCK_EnableUsbhsClock();

    // USB PHY calibration/trim.
#if ((!(defined FSL_FEATURE_SOC_CCM_ANALOG_COUNT)) && (!(defined FSL_FEATURE_SOC_ANATOP_COUNT)))
    USBPHY->TRIM_OVERRIDE_EN = 0x001fU; // override IFR value
#endif
    USBPHY->CTRL |= USBPHY_CTRL_SET_ENUTMILEVEL2_MASK | USBPHY_CTRL_SET_ENUTMILEVEL3_MASK;
    USBPHY->PWD = 0;

    uint32_t phytx = USBPHY->TX;
    phytx &= ~(USBPHY_TX_D_CAL_MASK | USBPHY_TX_TXCAL45DM_MASK | USBPHY_TX_TXCAL45DP_MASK);
    phytx |= USBPHY_TX_D_CAL(0x04) | USBPHY_TX_TXCAL45DP(0x07) | USBPHY_TX_TXCAL45DM(0x07);
    USBPHY->TX = phytx;

    // FreeRTOS + TinyUSB: IRQ priority must be numerically >= the max syscall
    // priority so tud_int_handler() can safely call FreeRTOS FromISR APIs.
    NVIC_SetPriority(USB1_HS_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
}

int NM2_GetCharNonBlocking(void)
{
    LPUART_Type *uart = (LPUART_Type *)BOARD_DEBUG_UART_BASEADDR;
    // Clear any overrun / framing / noise errors so the receiver doesn't stall.
    uint32_t flags = LPUART_GetStatusFlags(uart);
    if (flags & (uint32_t)(kLPUART_RxOverrunFlag | kLPUART_FramingErrorFlag | kLPUART_NoiseErrorFlag)) {
        LPUART_ClearStatusFlags(uart, kLPUART_RxOverrunFlag | kLPUART_FramingErrorFlag | kLPUART_NoiseErrorFlag);
    }
    // Use RXCOUNT (bytes actually in the RX FIFO) — matches LPUART_ReadBlocking.
    // Checking RDRF (kLPUART_RxDataRegFullFlag) can miss data when FIFO is enabled
    // and the watermark has not been reached yet.
    if (LPUART_GetRxFifoCount(uart) > 0U) {
        return static_cast<int>(LPUART_ReadByte(uart));
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Newlib retargeting: route printf / puts / fwrite → LPUART4 debug UART
//
// SessionTask.cpp and main.cpp include <cstdio> without fsl_debug_console.h,
// so the SDK's "printf → DbgConsole_Printf" macro never fires there.
// This strong _write() definition overrides the no-op stub in libnosys.a and
// ensures all newlib stdio output lands on the MCU-Link virtual COM port.
// ---------------------------------------------------------------------------
// Bytes discarded because the debug UART stopped draining. Non-zero means the
// console stalled at some point -- otherwise invisible, since the evidence of
// a stalled console is the absence of output.
extern "C" { volatile uint32_t gConsoleTxDropped = 0; }

extern "C" int _write(int /*fd*/, const char *buf, int len)
{
    // NOT LPUART_WriteBlocking(). That waits forever for TX space, and this
    // runs on the same task that services the console keys and the
    // NetworkMIDI2 session tick -- so a debug UART that stops draining takes
    // the bridge down with it, silently: USB MIDI keeps enumerating (its own
    // task and IRQ) while ESC stops working and the session eventually times
    // out. Observed three times on the bench, each needing a debug-probe reset
    // to clear.
    //
    // Diagnostics are not worth a bridge. If the console will not take a byte
    // within a bounded wait, the rest of the line is dropped and the caller
    // continues. The wait is generous next to one character time at 115200
    // (~87 us) but bounded to single-digit milliseconds, and only the first
    // character of a stalled write pays it -- the remainder returns at once.
    constexpr uint32_t kTxSpinLimit = 200000U;

    auto *uart = reinterpret_cast<LPUART_Type *>(BOARD_DEBUG_UART_BASEADDR);
    for (int i = 0; i < len; ++i) {
        uint32_t spins = 0;
        while ((LPUART_GetStatusFlags(uart) &
                static_cast<uint32_t>(kLPUART_TxDataRegEmptyFlag)) == 0U) {
            if (++spins >= kTxSpinLimit) {
                gConsoleTxDropped += static_cast<uint32_t>(len - i);
                // Report the full length as written: newlib retries a short
                // write, which would spin here again for every later byte.
                return len;
            }
        }
        LPUART_WriteByte(uart, static_cast<uint8_t>(buf[i]));
    }
    return len;
}

// ---------------------------------------------------------------------------
// FreeRTOS hook implementations
// ---------------------------------------------------------------------------

extern "C" {

// USB1 High-Speed controller IRQ -- forwards to TinyUSB core.
void USB1_HS_IRQHandler(void)
{
    tusb_int_handler(1, true);
}

void vAssertCalled(const char *pcFile, uint32_t ulLine)
{
    taskDISABLE_INTERRUPTS();
    PRINTF("\r\n[ASSERT] %s : %u\r\n", pcFile, (unsigned)ulLine);
    for (;;) {}
}

void vApplicationStackOverflowHook(TaskHandle_t /*task*/, char *taskName)
{
    PRINTF("\r\n[FATAL] Stack overflow: %s\r\n", taskName);
    for (;;) {}
}

void vApplicationMallocFailedHook(void)
{
    PRINTF("\r\n[FATAL] FreeRTOS heap exhausted\r\n");
    for (;;) {}
}

} // extern "C"
