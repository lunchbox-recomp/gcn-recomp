// Disc drive timing checks: the `test_drive` target. Runs with no disc, device or game.
//
// runtime/hw/drive.h times each DVD read as a console's drive would. Each check compares what
// it gives with a time worked out here from the drive's numbers: 600 us to start a read, a
// block under the head at the disc's rate for its radius, a block read ahead at 32 MiB/s and
// a seek for anything else. The reads the game sees at console speed are what keep a music
// stream's header and its first data in different 5 ms audio frames.
//
// usage: test_drive
// Exits 0 only if every check passes.
#include "hw/drive.h"
#include <cmath>
#include <cstdio>

namespace {

int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

bool near(double a, double b, double within = 1e-9) { return std::fabs(a - b) <= within; }

constexpr double kMiB = 0x100000;

// The byte offset whose track sits at radius `r`, from the density of a Wii disc's first
// layer: 0x118240000 bytes between 24 mm and 58 mm.
uint64_t at_radius(double r) { return (uint64_t)((r * r - 0.024 * 0.024) / (0.058 * 0.058 - 0.024 * 0.024) * 0x118240000); }

void disc_checks() {
    check(near(drive::radius(0), 0.024) && near(drive::radius(0x118240000), 0.058),
          "a Wii layer's first byte sits at 24 mm and its last at 58 mm");
    check(std::fabs(drive::radius(0x57058000) - 0.038) < 0.0001, "a full GameCube disc ends near 38 mm");

    // A block at the inner edge moves at 2.1 MiB/s, one whose middle is at 31 mm at the
    // average of 2.1 and 3.325, and one at 38 mm at 3.325.
    check(near(drive::disc_seconds(0, 0x8000), 0x8000 / (2.1 * kMiB), 1e-6), "a block at 24 mm takes 14.9 ms");
    check(near(drive::disc_seconds(at_radius(0.031) - 0x4000, 0x8000), 0x8000 / (2.7125 * kMiB), 1e-6),
          "a block whose middle is at 31 mm takes 11.5 ms");
    check(near(drive::disc_seconds(at_radius(0.038) - 0x4000, 0x8000), 0x8000 / (3.325 * kMiB), 1e-6),
          "a block whose middle is at 38 mm takes 9.4 ms");

    // A 2 mm move costs 75 ms and 4.5 ms a millimeter. Half a millimeter costs 35 ms and
    // 50 ms a millimeter, either way.
    check(near(drive::seek_seconds(0, at_radius(0.026)), 0.084, 1e-6), "a 2 mm seek takes 84 ms");
    check(near(drive::seek_seconds(at_radius(0.0245), 0), 0.060, 1e-6) &&
              near(drive::seek_seconds(0, at_radius(0.0245)), 0.060, 1e-6),
          "a half-millimeter seek takes 60 ms in either direction");

    // At 24 mm the spiral is 32432.43 turns out, so the first block is 0.43 of a turn from
    // where the disc starts, and the disc turns 28.5 times a second.
    const double first = std::fmod(0.024 / 0.74e-6, 1.0) / 28.5;
    check(near(drive::turn_seconds(0, 0), first), "the first block comes round 15.2 ms after time 0");
    check(near(drive::turn_seconds(0, first - 0.001), 0.001, 1e-7), "and 1 ms after a time 1 ms before that");
    check(near(drive::turn_seconds(0, first + 0.001), 1 / 28.5 - 0.001, 1e-7),
          "and a turn less 1 ms after a time 1 ms past it");
}

void read_checks() {
    // A drive that holds nothing has its head at 0, so the first block reads without a seek,
    // and so does the one after it.
    drive::Drive d;
    const double cold = 600e-6 + drive::disc_seconds(0, 0x8000);
    double t = d.read(0, 0, 0x20);
    check(near(t, cold), "a 32-byte read at offset 0 takes 600 us and a block at the disc's rate");
    check(t > 0.005, "which is longer than a 5 ms audio frame");
    check(near(d.read(t, 0x8000, 0x8000), 600e-6 + drive::disc_seconds(0x8000, 0x8000)),
          "the next block, read as the last one finishes, is under the head and not yet read ahead");

    // Long after, the drive has read ahead, so a block it holds moves at 32 MiB/s.
    drive::Drive ahead;
    ahead.read(0, 0, 0x20);
    check(near(ahead.read(10, 0x10000, 0x8000), 600e-6 + 0x8000 / (32 * kMiB)),
          "a block read ahead takes 600 us and 1 ms");
    check(near(ahead.read(20, 0x40000, 0x100), 600e-6 + 0x100 / (32 * kMiB)),
          "and 256 bytes of one take 600 us and 7.6 us");

    // While the drive reads ahead it holds the blocks it has reached. 3.5 blocks' time after
    // the first read, the blocks from 0x8000 to 0x1FFFF are held and 0x20000 is under the head.
    drive::Drive d1, d2;
    const double t1 = d1.read(0, 0, 0x20);
    d2.read(0, 0, 0x20);
    const double mid = t1 + 3.5 * drive::disc_seconds(0x8000, 0x8000);
    check(near(d1.read(mid, 0x18000, 0x8000), 600e-6 + 0x8000 / (32 * kMiB)),
          "a block the read ahead has reached is held while it reads on");
    check(near(d2.read(mid, 0x20000, 0x8000), 600e-6 + drive::disc_seconds(0x20000, 0x8000)),
          "and the block it reaches next is under the head");

    // 5.5 blocks' time on it holds up to 0x2FFFF. A read of 0x18000 leaves it reading on from
    // 0x30000, so 0x28000 is still held right after.
    drive::Drive d3;
    const double t3 = d3.read(0, 0, 0x20);
    const double later = t3 + 5.5 * drive::disc_seconds(0x8000, 0x8000);
    const double t4 = d3.read(later, 0x18000, 0x8000);
    check(d3.fill_from == 0x30000 && near(d3.read(later + t4, 0x28000, 0x8000), 600e-6 + 0x8000 / (32 * kMiB)),
          "a read inside what the drive holds keeps what it held past the read");

    // A read of no bytes still costs a block, and the drive reads ahead after it.
    drive::Drive none;
    check(near(none.read(0, 0, 0), cold) && none.fill_to == 0xF8000, "a read of 0 bytes costs a block and reads ahead");

    // A read that crosses a block's end reads both blocks.
    drive::Drive cross;
    check(near(cross.read(0, 0x7FF8, 0x10), cold + drive::disc_seconds(0x8000, 0x8000)),
          "16 bytes across a block's end read two blocks");

    // From a head at 0, a block at 26 mm is a 2 mm seek and then the wait for it to come round.
    drive::Drive far;
    const uint64_t at26 = at_radius(0.026) / 0x8000 * 0x8000;
    const double seek = 600e-6 + drive::seek_seconds(0, at26);
    check(near(far.read(0, at26, 0x20), seek + drive::turn_seconds(at26, seek)),
          "a read 2 mm out takes 600 us, the seek and the turn to its block");

    // A seek reads ahead 1 MiB from the block it read. A read of that block again leaves what
    // the drive holds alone, and a read behind it throws it away.
    const drive::Drive was = far;
    check(near(far.read(10, at26, 0x20), 600e-6 + 0x20 / (32 * kMiB)) && far.fill_from == was.fill_from &&
              far.fill_to == was.fill_to && far.fill_start == was.fill_start,
          "reading the block a seek read leaves the read ahead as it was");
    check(far.fill_to == at26 + 0x100000, "a seek reads ahead until it holds the 1 MiB from its block");
    check(far.read(20, at26 - 0x200000, 0x20) > 0.035, "a read 2 MiB behind it seeks");
    check(far.read(30, at26 + 0x10000, 0x20) > 0.035, "and the block 2 out from the seek is no longer held");

    // A read that ends two blocks into what's held leaves it alone too, and three blocks in
    // starts a new read ahead.
    drive::Drive two;
    two.read(0, at26, 0x20);
    const drive::Drive before = two;
    two.read(10, at26 + 0x8000, 0x20);
    check(two.fill_from == before.fill_from && two.fill_start == before.fill_start,
          "a read that ends two blocks into what's held leaves the read ahead as it was");
    two.read(20, at26 + 0x10000, 0x20);
    check(near(two.fill_start, 20 + 600e-6 + 0x20 / (32 * kMiB)), "and one that ends three blocks in starts a new one");
}

}  // namespace

int main() {
    disc_checks();
    read_checks();
    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
