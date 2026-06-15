/**
 * @file lwipopts.h
 * @brief lwIP configuration for NetworkMIDI2 Pico 2 W — FreeRTOS threadsafe_background.
 *
 * Identical to the polling (lwip/) variant except:
 *   MEM_LIBC_MALLOC = 0  — required; pico_cyw43_arch_lwip_threadsafe_background
 *                          asserts at init if 1, because malloc/free are not
 *                          re-entrant from the IRQ/alarm context lwIP runs in.
 *
 * All other settings (IGMP, mDNS, MEMP_NUM_SYS_TIMEOUT, etc.) are unchanged.
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
// OS / socket model — NO_SYS=1; lwIP driven from IRQ/alarm (threadsafe_background)
// ---------------------------------------------------------------------------
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// ---------------------------------------------------------------------------
// Memory
// MEM_LIBC_MALLOC=0 is MANDATORY with pico_cyw43_arch_lwip_threadsafe_background.
// The SDK asserts at cyw43_arch_init() if it is 1.
// ---------------------------------------------------------------------------
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24

// ---------------------------------------------------------------------------
// Network protocols
// ---------------------------------------------------------------------------
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_IGMP                   1   // required for mDNS multicast join

// ---------------------------------------------------------------------------
// TCP tuning
// ---------------------------------------------------------------------------
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

// ---------------------------------------------------------------------------
// Network interface callbacks
// ---------------------------------------------------------------------------
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

// ---------------------------------------------------------------------------
// DHCP / ARP behaviour
// ---------------------------------------------------------------------------
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// ---------------------------------------------------------------------------
// mDNS responder — _midi2._udp service advertisement (M2-124-UM §8.1)
// ---------------------------------------------------------------------------
#define LWIP_MDNS_RESPONDER         1
#define MDNS_MAX_SERVICES           2
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#define LWIP_DNS_SUPPORT_MDNS_QUERIES 1
#define LWIP_MDNS_SEARCH            1

// Pool of simultaneous active timeouts — 20 covers mDNS retransmissions.
#define MEMP_NUM_SYS_TIMEOUT        20

// ---------------------------------------------------------------------------
// Checksum
// ---------------------------------------------------------------------------
#define LWIP_CHKSUM_ALGORITHM       3

// ---------------------------------------------------------------------------
// Stats (only in debug builds)
// ---------------------------------------------------------------------------
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0

#ifndef NDEBUG
#define LWIP_DEBUG                  1
#define LWIP_STATS                  1
#define LWIP_STATS_DISPLAY          1
#endif
