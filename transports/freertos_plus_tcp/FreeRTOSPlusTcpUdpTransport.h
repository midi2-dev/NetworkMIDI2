/**
 * @file FreeRTOSPlusTcpUdpTransport.h
 * @brief IUdpTransport implementation for FreeRTOS-Plus-TCP (Phase 8).
 *
 * Uses FreeRTOS_socket / FreeRTOS_bind / FreeRTOS_recvfrom / FreeRTOS_sendto.
 * The socket is created with FREERTOS_SO_RCVTIMEO set to 0 so that receive()
 * returns immediately when no datagram is pending.
 *
 * Copyright (c) 2026 AmeNote Inc. All rights reserved.
 *
 * Part of the AmeNote NetworkMIDI2 binary distribution.
 * Free for educational, evaluation, and non-commercial development use.
 * Commercial use requires a license — visit https://amenote.com
 * See LICENSE for full terms.
 *
 * PROVIDED AS IS, WITHOUT WARRANTY OF ANY KIND. See LICENSE for the
 * full disclaimer.
 */

#pragma once
#include "networkmidi2/UdpTransport.h"

// FreeRTOS-Plus-TCP headers — available when building against that stack.
#include "FreeRTOS.h"
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"

namespace networkmidi2 {

/**
 * @brief FreeRTOS-Plus-TCP UDP transport.
 *
 * ## Integration
 * ```cpp
 * FreeRTOSPlusTcpUdpTransport transport;
 * NetworkMidiSession session(transport, info, callbacks);
 * session.beginHost(5004);
 * for (;;) {
 *     session.tick();
 *     vTaskDelay(pdMS_TO_TICKS(1));
 * }
 * ```
 */
class FreeRTOSPlusTcpUdpTransport : public IUdpTransport {
public:
    FreeRTOSPlusTcpUdpTransport();
    ~FreeRTOSPlusTcpUdpTransport() override;

    FreeRTOSPlusTcpUdpTransport(const FreeRTOSPlusTcpUdpTransport &)            = delete;
    FreeRTOSPlusTcpUdpTransport &operator=(const FreeRTOSPlusTcpUdpTransport &) = delete;

    bool     open(uint16_t localPort) override;
    void     close()                  override;
    bool     sendTo(const UdpEndpoint &dst, const uint8_t *data, size_t len) override;
    size_t   receive(UdpEndpoint &from, uint8_t *buf, size_t cap)            override;
    uint32_t nowMillis()                                                      override;

private:
    Socket_t sock_ = FREERTOS_INVALID_SOCKET;
};

} // namespace networkmidi2
