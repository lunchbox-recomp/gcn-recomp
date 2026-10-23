// SRAM save checks through the EXI device: the `test_exi` target. Runs with no disc image,
// device, SDL or game.
//
// runtime/hw/exi.cpp is compiled into this file, with hw/memcard.cpp, hw/sram.cpp and
// platform.cpp linked as they are. Each check drives the IPL device the way the guest's OS
// does, through exi_write32 and exi_read32 on channel 0: select the device, send the command
// word, move the bytes, deselect it. What it looks at is the SRAM file in the work dir.
//
// The guest writes the SRAM a span at a time in one selection of the device, so the file is
// written when the device is deselected, and only if the span changed what the file holds.
// A save that fails has to be tried again at the next write. The file is the replay's
// g_sram_path when that's set, and <save dir>/sram.raw when it isn't. test_sram covers the
// file's load and save on their own; none of this was reached by a test before.
//
// Stubbed: logging, guest memory, the clock, the event queue and the interrupt line.
//
// usage: test_exi <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if
// every check passes.
#include "hw/exi.cpp"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

uint8_t* g_mem;
bool g_log_enabled[LOG_COUNT];
void log_msg(LogCat, const char*, ...) {}
uint64_t now_ticks() { return 0; }
void event_schedule_in(uint64_t, EventFn) {}
void pi_set_interrupt(uint32_t, bool) {}

namespace {

int g_checks, g_failed;
std::string g_dir;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

std::string path_of(const char* name) { return g_dir + "/" + name; }

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// The file's bytes, or "<missing>".
std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "<missing>";
    std::string text;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

std::string bytes(const uint8_t* s) { return std::string((const char*)s, SRAM_SIZE); }

// The defaults with the sound mode set to mono, as a game's options screen leaves them. The
// flags byte is 0x28 instead of 0x2C, which makes the checksum pair over the words 0x0C to
// 0x13 0028, FFD4.
void mono(uint8_t* s) {
    sram_defaults(s);
    s[0x13] = 0x28;
    s[0] = 0x00;
    s[1] = 0x28;
    s[2] = 0xFF;
    s[3] = 0xD4;
}

// ---- the guest's side of channel 0 ----
constexpr uint32_t kCsr = 0x00, kCr = 0x0C, kData = 0x10;  // the channel's registers
constexpr uint32_t kSelectIpl = 2u << 7;                    // chip select 1, the IPL device
constexpr uint32_t kReadSram = 0x20000100, kWriteSram = 0xA0000100;

void select_ipl() { exi_write32(kCsr, kSelectIpl); }
void deselect() { exi_write32(kCsr, 0); }

// An immediate transfer of the first n bytes of `data`, high byte first.
void send(uint32_t data, int n) {
    exi_write32(kData, data);
    exi_write32(kCr, 1u | 1u << 2 | (uint32_t)(n - 1) << 4);
}

uint32_t receive(int n) {
    exi_write32(kCr, 1u | (uint32_t)(n - 1) << 4);
    return exi_read32(kData);
}

// The SRAM's 64 bytes from offset 0, which is where the OS starts every write it makes.
void send_sram(const uint8_t* s) {
    send(kWriteSram, 4);
    for (int i = 0; i < SRAM_SIZE; i += 4)
        send((uint32_t)s[i] << 24 | (uint32_t)s[i + 1] << 16 | (uint32_t)s[i + 2] << 8 | s[i + 3], 4);
}

void guest_write(const uint8_t* s) {
    select_ipl();
    send_sram(s);
    deselect();
}

void guest_read(uint8_t* s) {
    select_ipl();
    send(kReadSram, 4);
    for (int i = 0; i < SRAM_SIZE; i += 4) {
        const uint32_t w = receive(4);
        for (int j = 0; j < 4; j++) s[i + j] = (uint8_t)(w >> (24 - 8 * j));
    }
    deselect();
}

// A boot: the paths the frontend sets, then exi_init. The save dir is always one in the work
// dir, so a path that's ignored lands there and not in the working directory.
void boot(const std::string& sram_path, const char* save_dir) {
    static const std::string card = path_of("card.raw");
    static const std::string spare = path_of("spare");
    g_sram_path = sram_path.empty() ? nullptr : sram_path.c_str();
    g_save_dir = save_dir ? save_dir : spare.c_str();
    g_memcard_path = card.c_str();
    exi_init();
}

// ---- the checks ----
void read_only() {
    const std::string p = path_of("read.raw");
    boot(p, nullptr);
    uint8_t got[SRAM_SIZE], defaults[SRAM_SIZE];
    sram_defaults(defaults);
    guest_read(got);
    check(bytes(got) == bytes(defaults), "with no file the guest reads the default SRAM");
    check(!exists(p), "and a run that only reads leaves no file");
}

void changed_span() {
    const std::string p = path_of("changed.raw");
    boot(p, nullptr);
    uint8_t want[SRAM_SIZE];
    mono(want);
    check(sram_checksums_ok(want), "the mono SRAM's checksums are right");
    select_ipl();
    send_sram(want);
    check(!exists(p), "a changed span isn't saved while the device is still selected");
    deselect();
    check(read_file(p) == bytes(want), "and is saved, whole, when the device is deselected");

    remove(p.c_str());
    guest_write(want);
    check(!exists(p), "the same span written again is not saved a second time");
}

void unchanged_span() {
    const std::string p = path_of("unchanged.raw");
    boot(p, nullptr);
    uint8_t defaults[SRAM_SIZE];
    sram_defaults(defaults);
    guest_write(defaults);
    check(!exists(p), "a span that leaves the SRAM as it was saves no file");
}

void next_boot_reads_it() {
    const std::string p = path_of("boots.raw");
    boot(p, nullptr);
    uint8_t want[SRAM_SIZE], got[SRAM_SIZE];
    mono(want);
    guest_write(want);
    boot(p, nullptr);
    guest_read(got);
    check(bytes(got) == bytes(want) && (got[0x13] & 4) == 0, "the next boot reads the mono SRAM that was saved");
}

void path_choice() {
    const std::string save = path_of("save");
    const std::string replay = path_of("replay.raw");
    uint8_t want[SRAM_SIZE];
    mono(want);

    boot(replay, save.c_str());
    guest_write(want);
    check(read_file(replay) == bytes(want), "with an SRAM path set, the SRAM is saved there");
    check(!exists(save + "/sram.raw"), "and not in the save dir");

    boot("", save.c_str());
    guest_write(want);
    check(read_file(save + "/sram.raw") == bytes(want), "with none set, it's saved in the save dir as sram.raw");
}

void failed_save_is_tried_again() {
    const std::string p = path_of("retry.raw");
    boot(p, nullptr);
    uint8_t want[SRAM_SIZE];
    mono(want);
    mkdir((p + ".tmp").c_str(), 0755);
    guest_write(want);
    check(!exists(p), "a save that fails leaves no file");
    rmdir((p + ".tmp").c_str());
    guest_write(want);
    check(read_file(p) == bytes(want), "and the next write of the span saves it");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: test_exi <work dir>\n");
        return 2;
    }
    g_dir = argv[1];
    if (exists(g_dir) || mkdir(g_dir.c_str(), 0755) != 0) {
        fprintf(stderr, "%s exists or can't be made: give a new directory\n", g_dir.c_str());
        return 2;
    }
    read_only();
    changed_span();
    unchanged_span();
    next_boot_reads_it();
    path_choice();
    failed_save_is_tried_again();
    printf("test_exi: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
