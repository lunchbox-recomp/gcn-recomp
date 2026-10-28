// How long the disc drive takes over a read, as a console's does: where the head is, what the
// drive has read ahead and where on the disc the data sits. Free of the rest of the runtime,
// so tools/test_drive.cpp checks it on its own.
//
// The model and its numbers are Dolphin's, which were measured on hardware. A read command
// costs 600 us to start. The drive reads whole 32 KiB ECC blocks. A block the drive has
// already read ahead moves at 32 MiB/s. The block under the head costs one block at the
// disc's own rate, which runs from 2.1 MiB/s at the inner edge to 3.325 MiB/s at the outer,
// since the disc spins at a constant 28.5 turns a second. Any other block is a seek, which
// costs the head's travel and then the wait for the block to come round. After a read the
// drive keeps the last block, the one before it and everything after, and goes on reading
// until it holds 1 MiB.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace drive {

constexpr uint64_t kBlock = 0x8000;         // an ECC block, the least the drive reads
constexpr uint64_t kReadAhead = 0x100000;   // what the drive holds, read and read ahead
constexpr double kBufferRate = 32.0 * 0x100000;      // bytes a second from what it holds
constexpr double kReadStart = 600e-6;       // seconds before a read command starts reading
constexpr double kCommand = 300e-6;         // seconds any other command takes

// The track's radius in meters at a byte offset. The data density is a Wii disc's, whose
// first layer holds 0x118240000 bytes between 24 mm and 58 mm. A GameCube disc has the same
// density and ends near 38 mm.
inline double radius(uint64_t offset) {
    constexpr double kLayer = 0x118240000, kInner = 0.024, kOuter = 0.058;
    return std::sqrt((double)offset / kLayer * (kOuter * kOuter - kInner * kInner) + kInner * kInner);
}

// Seconds to read `len` bytes from the disc itself, at the rate where the read's middle sits.
inline double disc_seconds(uint64_t offset, uint64_t len) {
    constexpr double kInner = 0.024, kOuter = 0.038;
    constexpr double kInnerRate = 2.1 * 0x100000, kOuterRate = 3.325 * 0x100000;
    const double r = radius(offset + len / 2);
    return (double)len / (kInnerRate + (r - kInner) / (kOuter - kInner) * (kOuterRate - kInnerRate));
}

// Seconds to move the head between two offsets and read a block there. Moves under 1 mm are
// slower for their length.
inline double seek_seconds(uint64_t from, uint64_t to) {
    const double d = std::fabs(radius(from) - radius(to));
    return d < 0.001 ? 0.035 + d * 50 : 0.075 + d * 4.5;
}

// Seconds from time `at` until the disc has turned the block at `offset` under the head. The
// track is a spiral 0.74 um apart, and the disc has spun at a constant rate since time 0.
inline double turn_seconds(uint64_t offset, double at) {
    constexpr double kTurns = 28.5, kPitch = 0.74e-6;
    const double target = std::fmod(radius(offset) / kPitch, 1.0);
    const double now = std::fmod(at * kTurns, 1.0);
    return std::fmod(target - now + 1.0, 1.0) / kTurns;
}

struct Drive {
    // What the drive is reading ahead: blocks [fill_from, fill_to), from fill_start to
    // fill_end in seconds. The same start and end mean it holds nothing.
    uint64_t fill_from = 0, fill_to = 0;
    double fill_start = 0, fill_end = 0;

    // Seconds a read of `len` bytes at `offset` takes when the command starts at `now`
    // seconds, which is never before the last read finished. Updates what the drive holds.
    double read(double now, uint64_t offset, uint32_t len) {
        // What it holds now: up to a buffer's length behind the end of what it reads ahead,
        // and as far as it has reached by now. The head is parked where it stopped.
        uint64_t held_from = 0, held_to = 0, head = 0;
        if (fill_end != fill_start) {
            held_from = fill_to > kReadAhead ? fill_to - kReadAhead : 0;
            if (now >= fill_end) {
                held_to = fill_to;
            } else {
                const double part = (now - fill_start) / (fill_end - fill_start) * (double)(fill_to - fill_from);
                held_to = fill_from + (uint64_t)part / kBlock * kBlock;
            }
            head = held_to;
        }
        uint64_t block = offset / kBlock * kBlock;
        const uint64_t first = block;
        if (block < held_from) held_from = held_to = 0;  // reading behind it throws it away

        double t = kReadStart;
        bool seeked = false;
        uint64_t left = len;
        do {
            const uint64_t chunk = std::min(left, block + kBlock - offset);
            if (block >= held_from && block < held_to) {
                t += (double)chunk / kBufferRate;
            } else {
                if (block != head) {
                    seeked = true;
                    t += seek_seconds(head, block);
                    t += turn_seconds(block, now + t);
                } else {
                    t += disc_seconds(block, kBlock);
                }
                head = block + kBlock;
            }
            offset += chunk;
            left -= chunk;
            block += kBlock;
        } while (left > 0);

        // A read that ends within the first two blocks of what's held leaves it as it was.
        // Otherwise the drive reads ahead from where the read stopped, or from what it had
        // already reached if that's further.
        const uint64_t last = block;
        if (held_to == held_from || last - held_from > 2 * kBlock) {
            fill_from = std::max(last, held_to);
            fill_to = last + kReadAhead - 2 * kBlock;
            if (seeked) fill_to = std::max(fill_to, first + kReadAhead);
            fill_start = now + t;
            fill_end = fill_start + disc_seconds(fill_from, fill_to - fill_from);
        }
        return t;
    }
};

}  // namespace drive
