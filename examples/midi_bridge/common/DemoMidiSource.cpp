/**
 * @file DemoMidiSource.cpp
 * @brief DemoMidiSource implementation.
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

#include "DemoMidiSource.h"

namespace nm2_example {

// Static constexpr arrays must have a definition when ODR-used (pre-C++17 inline).
constexpr uint8_t DemoMidiSource::kNotes[12];

unsigned DemoMidiSource::next(uint32_t nowMs, uint32_t words[2])
{
    if (firstTick_) {
        lastMs_    = nowMs;
        firstTick_ = false;
        return 0;
    }

    uint32_t elapsed = nowMs - lastMs_;

    if (!noteOn_) {
        if (elapsed < kOnIntervalMs) return 0;

        uint8_t note = kNotes[noteIdx_];
        // MT4 (MIDI 2.0 Channel Voice), group=0, status=0x90 (Note-On ch0)
        words[0] = (0x40u << 24) | (0x90u << 16) | (static_cast<uint32_t>(note) << 8);
        words[1] = 0xFFFF0000u;  // velocity = max (0xFFFF), attribute = 0
        lastMs_  = nowMs;
        noteOn_  = true;
        return 2;
    } else {
        if (elapsed < kOffDelayMs) return 0;

        uint8_t note = kNotes[noteIdx_];
        // Note-Off: same structure, status byte 0x80
        words[0] = (0x40u << 24) | (0x80u << 16) | (static_cast<uint32_t>(note) << 8);
        words[1] = 0x00000000u;
        lastMs_  = nowMs;
        noteOn_  = false;
        noteIdx_ = (noteIdx_ + 1u) % 12u;
        return 2;
    }
}

} // namespace nm2_example
