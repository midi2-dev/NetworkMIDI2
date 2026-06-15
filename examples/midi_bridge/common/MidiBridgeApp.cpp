/**
 * @file MidiBridgeApp.cpp
 * @brief MidiBridgeApp implementation — event loop, callbacks, UMP printing.
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

#include "MidiBridgeApp.h"
#include "networkmidi2/Types.h"
#include <csignal>
#include <cstdio>
#include <cstring>
#include <unistd.h>   // usleep

using namespace networkmidi2;

namespace nm2_example {

// ---------------------------------------------------------------------------
// SIGINT handling — signal handlers must be trivial; we only set a flag here.
// ---------------------------------------------------------------------------

static volatile sig_atomic_t gShutdown = 0;
static void sigintHandler(int) { gShutdown = 1; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const char *stateName(SessionState s)
{
    switch (s) {
    case SessionState::Idle:              return "Idle";
    case SessionState::PendingInvitation: return "PendingInvitation";
    case SessionState::AuthRequired:      return "AuthRequired";
    case SessionState::Established:       return "Established";
    case SessionState::PendingReset:      return "PendingReset";
    case SessionState::PendingBye:        return "PendingBye";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// MidiBridgeApp
// ---------------------------------------------------------------------------

MidiBridgeApp::MidiBridgeApp(const char *name, const char *productId)
{
    info_.setName(name);
    info_.setProductId(productId);
}

void MidiBridgeApp::runHost(uint16_t port)
{
    role_ = "nm2-host";
    NetworkMidiSession::Callbacks cb;
    cb.onUmp         = &MidiBridgeApp::onUmp;
    cb.onStateChange = &MidiBridgeApp::onStateChange;
    cb.ctx           = this;

    NetworkMidiSession session(transport_, info_, cb);
    session.beginHost(port, nullptr, auth_);

    if (discovery_) {
        if (discovery_->advertise(port, info_))
            printf("[%s] mDNS: advertised \"%s\" on port %u\n", role_, info_.name, port);
        else
            printf("[%s] mDNS: advertise failed (no DNS-SD daemon?)\n", role_);
    }

    printf("[%s] Listening on UDP port %u%s...\n", role_, port,
           auth_ ? " (auth required)" : "");
    session_ = &session;
    eventLoop(session, role_);

    if (discovery_) discovery_->unadvertise();
    session_ = nullptr;
}

void MidiBridgeApp::runClient(uint32_t hostIp, uint16_t hostPort, uint16_t localPort)
{
    role_ = "nm2-client";

    // If discovery is set, browse until a peer is found, then override
    // hostIp/hostPort with the discovered values.
    if (discovery_) {
        printf("[%s] mDNS: browsing for \"%s\" services...\n", role_, kMdnsServiceType);
        if (!discovery_->browse()) {
            printf("[%s] mDNS: browse failed (no DNS-SD daemon?) — falling back to direct IP\n",
                   role_);
        } else {
            DiscoveredPeer peer{};
            // Poll with a 10 s timeout; tick every 10 ms.
            for (int i = 0; i < 1000; ++i) {
                if (discovery_->nextDiscovered(peer)) {
                    hostIp   = peer.endpoint.ipv4;
                    hostPort = peer.endpoint.port;
                    printf("[%s] mDNS: found \"%s\" at %u.%u.%u.%u:%u\n",
                           role_, peer.epName,
                           (hostIp >> 24) & 0xFF, (hostIp >> 16) & 0xFF,
                           (hostIp >>  8) & 0xFF,  hostIp        & 0xFF,
                           hostPort);
                    break;
                }
                ::usleep(10000);
            }
            discovery_->stopBrowse();
        }
    }

    NetworkMidiSession::Callbacks cb;
    cb.onUmp         = &MidiBridgeApp::onUmp;
    cb.onStateChange = &MidiBridgeApp::onStateChange;
    cb.ctx           = this;

    UdpEndpoint hostEp{hostIp, hostPort};
    NetworkMidiSession session(transport_, info_, cb);
    session.beginClient(hostEp, localPort, auth_);
    printf("[%s] Connecting to %u.%u.%u.%u:%u from local port %u...\n",
           role_,
           (hostIp >> 24) & 0xFF, (hostIp >> 16) & 0xFF,
           (hostIp >>  8) & 0xFF,  hostIp        & 0xFF,
           hostPort, localPort);
    session_ = &session;
    eventLoop(session, role_);
    session_ = nullptr;
}

void MidiBridgeApp::eventLoop(NetworkMidiSession &session, const char *role)
{
    ::signal(SIGINT, sigintHandler);
    gShutdown = 0;

    bool hadConnection = false;

    while (true) {
        session.tick();
        SessionState st = session.state();

        // Feed demo MIDI while the session is Established.
        if (st == SessionState::Established) {
            hadConnection = true;
            uint32_t w[2];
            if (unsigned wc = demoSrc_.next(transport_.nowMillis(), w)) {
                session.sendUmp(w, wc);
            }
        }

        // Ctrl-C: initiate a clean Bye/ByeReply shutdown.
        if (gShutdown == 1) {
            printf("[%s] SIGINT — closing session...\n", role);
            if (st != SessionState::Idle) session.close();
            gShutdown = 2;
        }

        // Exit when the session returns to Idle.
        bool shouldExit = (hadConnection && st == SessionState::Idle)
                       || (gShutdown == 2 && st == SessionState::Idle);
        // Also exit immediately if Ctrl-C arrived before we connected.
        if (gShutdown == 2 && !hadConnection) shouldExit = true;
        if (shouldExit) break;

        ::usleep(1000);  // ≈ 1 ms per tick
    }

    printf("[%s] Done.\n", role);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void MidiBridgeApp::onUmp(void *ctx, const uint32_t *words, size_t count)
{
    auto *self = static_cast<MidiBridgeApp *>(ctx);
    printUmp(self->role_, words, count);
}

void MidiBridgeApp::onStateChange(void *ctx, SessionState newState)
{
    auto *self = static_cast<MidiBridgeApp *>(ctx);
    printf("[%s] State → %s", self->role_, stateName(newState));
    if (newState == SessionState::Established && self->session_) {
        printf("  (peer: \"%s\")", self->session_->remoteEpName());
    }
    printf("\n");
}

void MidiBridgeApp::printUmp(const char *prefix, const uint32_t *w, size_t n)
{
    if (n == 0) return;
    unsigned mt  = (w[0] >> 28) & 0xF;
    unsigned grp = (w[0] >> 24) & 0xF;

    printf("[%s] ← MT%u grp%u", prefix, mt, grp);

    if (mt == 0x4 && n >= 2) {
        // MT4: MIDI 2.0 Channel Voice message (2 words).
        unsigned status = (w[0] >> 16) & 0xFF;
        unsigned chan   = status & 0x0F;
        unsigned note   = (w[0] >> 8) & 0x7F;
        unsigned vel    = (w[1] >> 16) & 0xFFFF;
        const char *type;
        switch (status >> 4) {
        case 0x8: type = "Note-Off"; break;
        case 0x9: type = (vel > 0) ? "Note-On" : "Note-On(v=0)"; break;
        case 0xA: type = "Poly-Pressure"; break;
        case 0xB: type = "Control-Change"; break;
        case 0xC: type = "Program-Change"; break;
        case 0xD: type = "Chan-Pressure"; break;
        case 0xE: type = "Pitch-Bend"; break;
        case 0xF: type = "Per-Note"; break;
        default:  type = "ChanVoice"; break;
        }
        printf(" %s ch%u note=%u vel=0x%04X", type, chan, note, vel);
    } else if (mt == 0x2 && n >= 1) {
        // MT2: MIDI 1.0 Channel Voice message (1 word).
        unsigned status = (w[0] >> 16) & 0xFF;
        printf(" MIDI1 status=0x%02X", status);
    } else {
        // Fallback: print raw hex words.
        for (size_t i = 0; i < n; ++i) printf(" %08X", w[i]);
    }
    printf("\n");
}

} // namespace nm2_example
