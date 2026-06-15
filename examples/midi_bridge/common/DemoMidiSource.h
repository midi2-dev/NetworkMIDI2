/**
 * @file DemoMidiSource.h
 * @brief Generates a simple demo sequence of MIDI 2.0 note messages.
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
#include <cstdint>

namespace nm2_example {

/**
 * @brief Non-blocking demo MIDI note generator.
 *
 * Call next() from the event loop every tick.  It returns 0 most of the time;
 * when a note is due it fills @p words and returns the word count (always 2 for
 * MT4 MIDI 2.0 Channel Voice messages).
 *
 * The generated sequence cycles through a simple pentatonic scale ascending and
 * descending, alternating Note-On and Note-Off at a slow tempo so the output is
 * easy to read in a terminal.
 *
 * ## MIDI 2.0 Note-On format (MT4, group 0, channel 0)
 * ```
 * word[0] = 0x4090nn00   // MT=4, grp=0, status=0x90 (NoteOn ch0), note=nn, attrib=0
 * word[1] = 0xVVVV0000   // velocity (16-bit), attribute value = 0
 * ```
 * Note-Off is identical with status byte 0x80.
 */
struct DemoMidiSource {
    /** Gap between Note-On events (ms). */
    static constexpr uint32_t kOnIntervalMs  = 500;
    /** Gate time: Note-Off this many ms after the preceding Note-On. */
    static constexpr uint32_t kOffDelayMs    = 200;

    /**
     * @brief Check whether a message is due.
     * @param nowMs   Current time from IUdpTransport::nowMillis().
     * @param words   Output buffer — caller must provide at least 2 entries.
     * @return Word count (2 when a message is ready, 0 if nothing is due).
     */
    unsigned next(uint32_t nowMs, uint32_t words[2]);

private:
    uint32_t lastMs_    = 0;
    bool     firstTick_ = true;
    bool     noteOn_    = false;   // true = pending Note-Off
    unsigned noteIdx_   = 0;

    // Pentatonic scale (C4–B4 subset), ascending then descending.
    static constexpr uint8_t kNotes[12] = {60,62,64,67,69,72,69,67,64,62,60,62};
};

} // namespace nm2_example
