/**
 * @file SysExStreamGuard.h
 * @brief Keeps System Exclusive whole across a break in a delivered UMP stream.
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

#include <cstdint>

namespace networkmidi2 {
/**
 * @brief Keeps System Exclusive whole across a break in the delivered stream.
 *
 * When a receiver gives up on missing commands (the sender cannot retransmit
 * them), the messages either side of the hole are delivered back to back. A
 * SysEx that straddles the hole then runs straight on into the remains of
 * whatever followed it: its head joined to another message's tail, delivered as
 * one message with a wrong length, a bad checksum, or the next message's
 * identity -- measured against macOS as bad checksums and messages arriving
 * out of order at 4 KB SysEx.
 *
 * The rule this applies: a SysEx that loses any part is dropped whole, and one
 * that has already started downstream is ended there, incomplete, with a
 * zero-length End.
 *
 * - breakStream() at the hole: every open SysEx gets its End now.
 * - admit() on every message delivered after that: a Continue or End with no
 *   open Start (its head was in the hole, or it was just ended) is dropped, so
 *   the rest of a broken message never reaches the receiver.
 * - A Start or Complete arriving while its SysEx is still open ends the open
 *   one first -- the stream can only get there by losing that message's End.
 *
 * SysEx7 (MT 0x3) is tracked per Group, SysEx8 (MT 0x5) per Group and Stream ID.
 */
class SysExStreamGuard {
public:
    /** Receives the zero-length End packets the guard produces. */
    using EmitFn = void (*)(void *ctx, const uint32_t *words, uint8_t count);

    void reset()
    {
        for (unsigned g = 0; g < kGroups; ++g) {
            open7_[g] = false;
            for (unsigned i = 0; i < kStreamBytes; ++i) open8_[g][i] = 0;
        }
    }

    /** False if this message must not be delivered. May emit an End first. */
    bool admit(const uint32_t *w, EmitFn emit, void *ctx)
    {
        const uint8_t mt = (w[0] >> 28) & 0x0Fu;
        if (mt != 0x3u && mt != 0x5u) return true;
        const uint8_t g  = (w[0] >> 24) & 0x0Fu;
        const uint8_t st = (w[0] >> 20) & 0x0Fu;
        const uint8_t id = (w[0] >> 8) & 0xFFu;
        const bool    x8 = (mt == 0x5u);
        const bool    isOpen = x8 ? open8(g, id) : open7_[g];

        switch (st) {
        case kComplete:
        case kStart:
            if (isOpen) { endOpen(x8, g, id, emit, ctx); ++cutOff_; }
            setOpen(x8, g, id, st == kStart);
            return true;
        case kContinue:
            if (!isOpen) { ++orphans_; return false; }
            return true;
        case kEnd:
            if (!isOpen) { ++orphans_; return false; }
            setOpen(x8, g, id, false);
            return true;
        default:
            return true;                          // reserved status: not ours to judge
        }
    }

    /** The delivered stream has a hole: end every SysEx open downstream. */
    void breakStream(EmitFn emit, void *ctx)
    {
        for (uint8_t g = 0; g < kGroups; ++g) {
            if (open7_[g]) { endOpen(false, g, 0, emit, ctx); open7_[g] = false; ++cutOff_; }
            for (unsigned i = 0; i < kStreamBytes; ++i) {
                if (!open8_[g][i]) continue;
                for (uint8_t b = 0; b < 8; ++b) {
                    if (!(open8_[g][i] & (1u << b))) continue;
                    endOpen(true, g, static_cast<uint8_t>(i * 8u + b), emit, ctx);
                    ++cutOff_;
                }
                open8_[g][i] = 0;
            }
        }
    }

    /** SysEx ended early downstream (zero-length End sent for it). */
    uint32_t cutOff() const { return cutOff_; }
    /** Continue/End packets dropped because their message had no open Start. */
    uint32_t orphans() const { return orphans_; }

private:
    static constexpr unsigned kGroups      = 16;
    static constexpr unsigned kStreamBytes = 256 / 8;
    static constexpr uint8_t  kComplete = 0x0;
    static constexpr uint8_t  kStart    = 0x1;
    static constexpr uint8_t  kContinue = 0x2;
    static constexpr uint8_t  kEnd      = 0x3;

    bool open8(uint8_t g, uint8_t id) const { return open8_[g][id >> 3] & (1u << (id & 7u)); }

    void setOpen(bool x8, uint8_t g, uint8_t id, bool on)
    {
        if (!x8) { open7_[g] = on; return; }
        if (on) open8_[g][id >> 3] = static_cast<uint8_t>(open8_[g][id >> 3] |  (1u << (id & 7u)));
        else    open8_[g][id >> 3] = static_cast<uint8_t>(open8_[g][id >> 3] & ~(1u << (id & 7u)));
    }

    // Same Message Type, Group and (SysEx8) Stream ID, status End, no data.
    // A SysEx8 byte count includes the Stream ID byte, so it is 1. Reserved
    // bytes are zero, as the spec requires.
    static void endOpen(bool x8, uint8_t g, uint8_t id, EmitFn emit, void *ctx)
    {
        if (!emit) return;
        if (x8) {
            const uint32_t end8[4] = {(0x5u << 28) | (uint32_t(g) << 24) | (uint32_t(kEnd) << 20)
                                      | (1u << 16) | (uint32_t(id) << 8), 0, 0, 0};
            emit(ctx, end8, 4);
        } else {
            const uint32_t end7[2] = {(0x3u << 28) | (uint32_t(g) << 24) | (uint32_t(kEnd) << 20), 0};
            emit(ctx, end7, 2);
        }
    }

    bool     open7_[kGroups]               = {};
    uint8_t  open8_[kGroups][kStreamBytes] = {};
    uint32_t cutOff_  = 0;
    uint32_t orphans_ = 0;
};


} // namespace networkmidi2
