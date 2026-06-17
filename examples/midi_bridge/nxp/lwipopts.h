/**
 * @file lwipopts.h
 * @brief lwIP configuration for NetworkMIDI2 NXP FRDM-MCXN947 example.
 *
 * NO_SYS=0 (FreeRTOS tcpip_thread mode) with LWIP_TCPIP_CORE_LOCKING=1.
 * All lwIP raw API calls from application tasks must hold LOCK_TCPIP_CORE().
 * NxpUdpTransport and NxpMdnsDiscovery acquire the lock internally.
 *
 * Ethernet driver: NXP ENET_QOS (ENET_QOS_Drv) via ethernetif.c in the
 * MCUXpresso SDK.  Netif init function: ethernetif_init() (in ethernetif.c).
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

#pragma once

// ---------------------------------------------------------------------------
// OS / socket model — FreeRTOS tcpip_thread (NO_SYS=0)
// ---------------------------------------------------------------------------
#define NO_SYS                          0
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0

// ---------------------------------------------------------------------------
// Thread-safety model
// LWIP_TCPIP_CORE_LOCKING=1: application tasks acquire sys_mutex before
// calling any lwIP raw API.  NxpUdpTransport / NxpMdnsDiscovery do this
// internally via LOCK_TCPIP_CORE() / UNLOCK_TCPIP_CORE().
// SYS_LIGHTWEIGHT_PROT is disabled because the core lock provides all
// the required mutual exclusion between tasks and tcpip_thread.
// ---------------------------------------------------------------------------
#define LWIP_TCPIP_CORE_LOCKING         1
#define SYS_LIGHTWEIGHT_PROT            1

// Core-locking hooks — implemented in NXP sys_arch/dynamic/sys_arch.c
// Declare with C linkage so C++ translation units don't mangle the name.
#ifdef __cplusplus
extern "C" {
#endif
void sys_lock_tcpip_core(void);
void sys_unlock_tcpip_core(void);
void sys_check_core_locking(void);
void sys_mark_tcpip_thread(void);
#ifdef __cplusplus
}
#endif
#define LOCK_TCPIP_CORE()               sys_lock_tcpip_core()
#define UNLOCK_TCPIP_CORE()             sys_unlock_tcpip_core()
#define LWIP_ASSERT_CORE_LOCKED()       sys_check_core_locking()
#define LWIP_MARK_TCPIP_THREAD()        sys_mark_tcpip_thread()

// ---------------------------------------------------------------------------
// tcpip_thread configuration
// Priority 8 is above the session task (priority 2) so incoming packets are
// processed promptly.  Increase if needed.
// ---------------------------------------------------------------------------
#define TCPIP_THREAD_NAME               "lwIP"
#define TCPIP_THREAD_STACKSIZE          ( 1024 )
#define TCPIP_THREAD_PRIO               8
#define TCPIP_MBOX_SIZE                 32

// ---------------------------------------------------------------------------
// Default mailbox sizes (raw/UDP/TCP receive queues inside tcpip_thread)
// ---------------------------------------------------------------------------
#define DEFAULT_RAW_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       8
#define DEFAULT_ACCEPTMBOX_SIZE         4

// ---------------------------------------------------------------------------
// Memory
// lwIP internal heap; MEM_LIBC_MALLOC=0 is required for thread safety.
// Increase MEM_SIZE if you see MEMP_OVERFLOW or pbuf allocation failures.
// ---------------------------------------------------------------------------
#define MEM_LIBC_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        ( 16 * 1024 )
#define MEMP_NUM_TCP_SEG                32
#define MEMP_NUM_ARP_QUEUE              10
#define PBUF_POOL_SIZE                  32

// Polling interval for PHY link state (ms). Set >0 to use polling instead of
// GPIO interrupt — avoids needing EXAMPLE_PHY_INT_PORT/PIN configuration.
#define ETH_LINK_POLLING_INTERVAL_MS    1500

// Ethernet DMA ring sizes — must match BOARD_ENET_TX_BD_NUM / RX_BD_NUM
// in the NXP SDK board.h.  16 is the typical default for FRDM-MCXN947.
#define ETH_PAD_SIZE                    0

// Zero-copy RX path uses custom pbufs that wrap ENET DMA buffers.
#define LWIP_SUPPORT_CUSTOM_PBUF        1

// Disable IP reassembly — not needed for UDP MIDI and avoids a compile-time
// constraint in enet_configchecks.h (IP_REASS_MAX_PBUFS vs ENET buffer counts).
#define IP_REASSEMBLY                   0
#define IP_FRAG                         0

// ---------------------------------------------------------------------------
// Network protocols
// ---------------------------------------------------------------------------
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_IPV4                       1
#define LWIP_TCP                        1   // required by lwIP core even if unused
#define LWIP_UDP                        1
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define LWIP_TCP_KEEPALIVE              0
#define LWIP_IGMP                       1   // required for mDNS multicast

// ---------------------------------------------------------------------------
// Network interface callbacks
// ---------------------------------------------------------------------------
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_TX_SINGLE_PBUF       1
#define LWIP_NETIF_EXT_STATUS_CALLBACK  1  // required by ethernetif_wait_linkup()

// ---------------------------------------------------------------------------
// DHCP / ARP
// ---------------------------------------------------------------------------
#define DHCP_DOES_ARP_CHECK             0
#define LWIP_DHCP_DOES_ACD_CHECK        0

// ---------------------------------------------------------------------------
// mDNS responder + search — for _midi2._udp service (M2-124-UM §8.1)
// ---------------------------------------------------------------------------
#define LWIP_MDNS_RESPONDER             1
#define LWIP_MDNS_SEARCH                1
#define MDNS_MAX_SERVICES               2
#define LWIP_NUM_NETIF_CLIENT_DATA      2   // 1 for mDNS + 1 spare
#define LWIP_DNS_SUPPORT_MDNS_QUERIES   1

// Pool of simultaneous active timeouts — mDNS announce retransmissions
// consume several slots; 20 is sufficient.
#define MEMP_NUM_SYS_TIMEOUT            20

// ---------------------------------------------------------------------------
// Errno and random
// ---------------------------------------------------------------------------
// Provide errno values — arm-none-eabi newlib's errno.h may not export all
// POSIX values in a freestanding (-ffreestanding) build.
#define LWIP_PROVIDE_ERRNO              1

// LWIP_RAND is required when LWIP_IGMP=1 or LWIP_MDNS_RESPONDER=1.
// lwip_rand() is implemented in sys_arch/dynamic/sys_arch.c.
// Include stdint.h so uint32_t is available before u32_t is typedef'd by arch.h.
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t lwip_rand(void);
#ifdef __cplusplus
}
#endif
#define LWIP_RAND()                     lwip_rand()

// ---------------------------------------------------------------------------
// Checksum
// ---------------------------------------------------------------------------
#define LWIP_CHKSUM_ALGORITHM           3

// ---------------------------------------------------------------------------
// Stats (debug builds only)
// ---------------------------------------------------------------------------
#define MEM_STATS                       0
#define SYS_STATS                       0
#define MEMP_STATS                      0
#define LINK_STATS                      0

#ifndef NDEBUG
#define LWIP_DEBUG                      1
#define LWIP_STATS                      1
#define LWIP_STATS_DISPLAY              1
#endif
