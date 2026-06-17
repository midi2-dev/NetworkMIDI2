/**
 * @file SessionTask.h
 * @brief FreeRTOS task entry point for the Network MIDI 2.0 session (NXP MCXN947).
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
#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Main session task: waits for DHCP, then runs the session lifecycle and CLI.
 *
 * Unlike the Pico variant, there is no WiFi setup phase — the ENET_QOS
 * Ethernet link is hardware and initialised before the scheduler starts.
 * The task waits for a DHCP lease (up to 30 s), then prompts for:
 *   - mDNS hostname
 *   - Role (Host / Client)
 *   - Optional passphrase for SHA-256 authentication
 *
 * FEC drop simulation keys (once session is Established):
 *   d — drop 1 TX (FEC piggybacking recovers seamlessly)
 *   D — drop 3 TX (exceeds kFecDepth; receiver sends RetransmitRequest)
 *   r — drop 1 RX (receiver detects gap and sends RetransmitRequest)
 */
void vSessionTask(void *params);

#ifdef __cplusplus
}
#endif
