/**
 * @file Ump.h
 * @brief UMP packet framing: never hand a half-read packet to a session.
 *
 * A USB UMP FIFO is read in words, not messages. `tud_ump_read_ntoh(..., 4)`
 * and its host-role twin return up to four words and stop wherever the FIFO
 * runs dry, so a read can end in the middle of a multi-word packet. Passing
 * that straight to NetworkMidiSession::sendUmp() puts half a packet into a UMP
 * Data Command, and M2-124-UM section 7.2 is explicit:
 *
 *     "Each UMP packet shall be transmitted in whole within a UMP Data
 *      Command, not split across multiple UMP Data Commands."
 *
 * The receiver parses by Message Type, so a half packet makes it consume the
 * next command's first word to finish the one in hand, and the stream
 * desynchronises from there -- measured as truncated SysEx, out-of-order
 * messages and payloads that no longer parse at all.
 *
 * This is NOT about buffering a SysEx message. A SysEx7 message is a long run
 * of 2-word packets and nothing here holds it; the most this ever carries is
 * three words -- twelve bytes -- of an incomplete packet, kept until the rest
 * of it arrives on a later read.
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

#include <cstddef>
#include <cstdint>

namespace networkmidi2 {

/**
 * @brief Words in the UMP packet beginning with @p firstWord.
 *
 * From the Message Type in bits 31:28. Unrecognised types are treated as the
 * 4-word maximum, which keeps a framer from splitting something it does not
 * recognise rather than guessing it is short.
 */
inline uint8_t umpWordCount(uint32_t firstWord)
{
    switch ((firstWord >> 28) & 0x0Fu) {
    case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;  // utility, system, MIDI 1.0 CV
    case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;  // SysEx7, MIDI 2.0 CV
    case 0xB: case 0xC:                               return 3;  // reserved 96-bit
    default:                                          return 4;  // SysEx8, Flex, Stream
    }
}

/**
 * @brief Re-frames word-oriented reads into whole UMP packets.
 *
 * Feed it whatever a read returned; take whole packets out. An incomplete tail
 * stays until the words that finish it arrive.
 *
 * A packet the caller could not dispose of (a full session transmit FIFO, say)
 * is simply not consumed, so it is offered again on the next pass. That keeps
 * backpressure where it belongs -- in the USB FIFO, where the sender can feel
 * it -- instead of turning a slow link into silent loss.
 */
class UmpFramer {
public:
    /** Words held, complete or not. */
    size_t pending() const { return n_; }

    /** Room for another read of @p words words. */
    bool hasRoomFor(size_t words) const { return n_ + words <= kCapacity; }

    /** Take words from a read. Returns false if they would not fit, which
     *  only happens if the caller reads without draining first. */
    bool push(const uint32_t *words, size_t count)
    {
        if (!hasRoomFor(count)) return false;
        for (size_t i = 0; i < count; ++i) buf_[n_++] = words[i];
        return true;
    }

    /** The next complete packet, or 0 words if the head is still incomplete.
     *  The pointer stays valid until the next push() or consume(). */
    uint8_t peek(const uint32_t **out) const
    {
        if (n_ == 0) return 0;
        const uint8_t need = umpWordCount(buf_[0]);
        if (need > n_) return 0;          // incomplete: wait for the rest
        *out = buf_;
        return need;
    }

    /** Drop @p words words from the head, after disposing of them. */
    void consume(uint8_t words)
    {
        if (words >= n_) { n_ = 0; return; }
        for (uint8_t i = 0; i + words < n_; ++i) buf_[i] = buf_[i + words];
        n_ = static_cast<uint8_t>(n_ - words);
    }

    void reset() { n_ = 0; }

private:
    // Three carried words at most, plus one four-word read, plus a packet the
    // caller declined to take. Eight covers every combination.
    static constexpr uint8_t kCapacity = 8;
    uint32_t buf_[kCapacity] = {};
    uint8_t  n_              = 0;
};


/**
 * @brief Abandons a whole System Exclusive message when part of it cannot be sent.
 *
 * A bridge that cannot hand a UMP to USB has to drop it. Dropping a single
 * SysEx *Continue* is the worst of the options: the Start already went out, so
 * the receiver is left assembling a message that silently loses its middle and
 * ends short. It was measured doing exactly that -- 95 of 99 messages
 * truncated once the USB side could not keep up with 4 KB SysEx at ~70 KB/s.
 *
 * So once any packet of a SysEx is refused, the rest of that message is
 * suppressed too, and the message is ended cleanly if it had already started:
 *
 * - refused at Start: nothing was delivered, so the remainder is simply
 *   suppressed and the receiver never sees the message at all -- one clean
 *   loss instead of a corrupt one.
 * - refused at Continue or End: the receiver has an open message, so when the
 *   real End arrives a **zero-length End** is offered in its place. That ends
 *   the message properly rather than leaving the receiver's assembler open
 *   until some later message happens to close it.
 *
 * The result is that a busy USB side costs whole messages, which an
 * application can detect, rather than corrupted ones, which it cannot.
 *
 * SysEx7 (MT 0x3) is tracked per Group. SysEx8 (MT 0x5) carries a Stream ID
 * and several streams may be in flight on one Group, so one abandoned Stream
 * ID per Group is tracked; a second concurrent SysEx8 abandonment on the same
 * Group is not suppressed, which is conservative -- it degrades to the old
 * behaviour rather than suppressing a stream that is still healthy.
 */
class SysExDropGate {
public:
    void reset()
    {
        for (unsigned g = 0; g < kGroups; ++g) {
            state_[g].abandoning = false;
            state_[g].owesEnd    = false;
            state_[g].streamId   = 0;
            state_[g].isSysEx8   = false;
        }
    }

    /** True if this packet belongs to a message already being abandoned. */
    bool suppress(uint32_t firstWord) const
    {
        Kind k = classify(firstWord);
        if (k.kind == Kind::Other) return false;
        const Slot &s = state_[k.group];
        if (!s.abandoning) return false;
        if (s.isSysEx8 != (k.kind == Kind::SysEx8)) return false;
        if (k.kind == Kind::SysEx8 && s.streamId != k.streamId) return false;
        return true;
    }

    /** Call when a packet could not be written. Arms suppression if it was
     *  part of a multi-packet SysEx. */
    void noteRefused(uint32_t firstWord)
    {
        Kind k = classify(firstWord);
        if (k.kind == Kind::Other) return;            // single message; nothing to abandon
        if (k.status == kComplete) return;            // one-packet SysEx: nothing follows
        Slot &s = state_[k.group];
        s.abandoning = true;
        s.isSysEx8   = (k.kind == Kind::SysEx8);
        s.streamId   = k.streamId;
        // A Start that never went out leaves nothing open at the receiver.
        s.owesEnd    = (k.status != kStart);
    }

    /** Call when a packet was written, to keep the open/closed state honest. */
    void noteSent(uint32_t firstWord)
    {
        Kind k = classify(firstWord);
        if (k.kind == Kind::Other) return;
        Slot &s = state_[k.group];
        if (k.status == kEnd || k.status == kComplete) {
            s.abandoning = false;
            s.owesEnd    = false;
        }
    }

    /** When the real End of an abandoned message arrives, produce a
     *  zero-length End to close it. Returns 0 if nothing should be sent.
     *  Clears the abandoned state either way. */
    uint8_t terminatorFor(uint32_t firstWord, uint32_t out[2])
    {
        Kind k = classify(firstWord);
        if (k.kind == Kind::Other) return 0;
        Slot &s = state_[k.group];
        if (!s.abandoning) return 0;
        if (k.status != kEnd && k.status != kComplete) return 0;

        const bool owed = s.owesEnd;
        s.abandoning = false;
        s.owesEnd    = false;
        if (!owed) return 0;                          // receiver never saw a Start

        // Same Message Type, Group and (for SysEx8) Stream ID, status End,
        // zero data bytes. Reserved bytes are zero, as the spec requires.
        if (k.kind == Kind::SysEx8) {
            out[0] = (0x5u << 28) | (uint32_t(k.group) << 24) | (uint32_t(kEnd) << 20)
                   | (1u << 16) | (uint32_t(k.streamId) << 8);
            out[1] = 0;
        } else {
            out[0] = (0x3u << 28) | (uint32_t(k.group) << 24) | (uint32_t(kEnd) << 20);
            out[1] = 0;
        }
        return 2;
    }

private:
    static constexpr unsigned kGroups   = 16;
    static constexpr uint8_t  kComplete = 0x0;
    static constexpr uint8_t  kStart    = 0x1;
    static constexpr uint8_t  kContinue = 0x2;
    static constexpr uint8_t  kEnd      = 0x3;

    struct Kind {
        enum { Other, SysEx7, SysEx8 } kind = Other;
        uint8_t group    = 0;
        uint8_t status   = 0;
        uint8_t streamId = 0;
    };

    static Kind classify(uint32_t w)
    {
        Kind k;
        const uint8_t mt = (w >> 28) & 0x0Fu;
        if (mt != 0x3u && mt != 0x5u) return k;
        k.kind     = (mt == 0x3u) ? Kind::SysEx7 : Kind::SysEx8;
        k.group    = (w >> 24) & 0x0Fu;
        k.status   = (w >> 20) & 0x0Fu;
        k.streamId = (mt == 0x5u) ? uint8_t((w >> 8) & 0xFFu) : 0u;
        return k;
    }

    struct Slot {
        bool    abandoning = false;
        bool    owesEnd    = false;
        bool    isSysEx8   = false;
        uint8_t streamId   = 0;
    };
    Slot state_[kGroups] = {};
};

} // namespace networkmidi2
