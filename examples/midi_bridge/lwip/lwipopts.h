/**
 * @file lwipopts.h
 * @brief lwIP configuration for NetworkMIDI2 Pico 2 W example — polling mode (NO_SYS=1).
 *
 * Derived from pico-examples/pico_w/wifi/lwipopts_examples_common.h with mDNS
 * responder and IGMP added for _midi2._udp advertisement.
 *
 * This file must be on the compiler include path before any lwIP headers.
 * The CMakeLists.txt achieves this by listing CMAKE_CURRENT_LIST_DIR first in
 * target_include_directories.
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
// OS / socket model — polling mode, raw API only
// ---------------------------------------------------------------------------
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// ---------------------------------------------------------------------------
// Memory — MEM_LIBC_MALLOC is valid only in polling (non-threadsafe) mode.
// Phase 9 lwipopts.h sets this to 0 for the FreeRTOS variant.
// ---------------------------------------------------------------------------
#define MEM_LIBC_MALLOC             1
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
// DHCP / ARP behaviour (matches pico-examples defaults)
// ---------------------------------------------------------------------------
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// ---------------------------------------------------------------------------
// mDNS responder — for _midi2._udp service advertisement (M2-124-UM §8.1)
// LWIP_NETIF_CLIENT_DATA is a slot count; mDNS calls netif_alloc_client_data_id()
// which requires at least 1 slot (the function is #ifdef'd out at 0).
// ---------------------------------------------------------------------------
#define LWIP_MDNS_RESPONDER         1
#define MDNS_MAX_SERVICES           2   // _midi2._udp + spare
// LWIP_NUM_NETIF_CLIENT_DATA: user-allocatable per-netif data slots.
// mdns_resp_init() calls netif_alloc_client_data_id() which requires at least 1.
// (Pico SDK 2.2.0 uses LWIP_NUM_NETIF_CLIENT_DATA, not the older LWIP_NETIF_CLIENT_DATA.)
#define LWIP_NUM_NETIF_CLIENT_DATA  1
// LWIP_DNS_SUPPORT_MDNS_QUERIES: route dns_gethostbyname("name.local") through
// mDNS multicast (224.0.0.251:5353) instead of the unicast DNS server.
// Required for client-side mDNS hostname resolution (Phase 8+ auto-connect).
#define LWIP_DNS_SUPPORT_MDNS_QUERIES 1

// LWIP_MDNS_SEARCH: enable the DNS-SD browse API (mdns_search_service).
// Used by LwipMdnsDiscovery::browse() to discover _midi2._udp peers via
// PTR→SRV→A chain, so Pico clients can auto-discover POSIX (Mac/Linux) hosts.
#define LWIP_MDNS_SEARCH            1

// MEMP_NUM_SYS_TIMEOUT: pool of simultaneous active timeouts.
// The SDK default (LWIP_NUM_SYS_TIMEOUT_INTERNAL) does not reserve slots for
// mDNS announce retransmissions.  20 is sufficient for all active modules.
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
