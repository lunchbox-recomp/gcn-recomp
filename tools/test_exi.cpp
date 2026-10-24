// SRAM and memory card save checks through the EXI device: the `test_exi` target. Runs with
// no disc image, device, SDL or game.
//
// runtime/hw/exi.cpp is compiled into this file, with hw/memcard.cpp, hw/sram.cpp and
// platform.cpp linked as they are. Each SRAM check drives the IPL device the way the guest's
// OS does, through exi_write32 and exi_read32 on channel 0: select the device, send the
// command word, move the bytes, deselect it. What it looks at is the SRAM file in the work dir.
//
// The guest writes the SRAM a span at a time in one selection of the device, so the file is
// written when the device is deselected, and only if the span changed what the file holds.
// A save that fails has to be tried again at the next write. The file is the replay's
// g_sram_path when that's set, and <save dir>/sram.raw when it isn't. test_sram covers the
// file's load and save on their own; none of this was reached by a test before.
//
// The card checks drive a MemCard with a shorter quiet period the same way, a page program
// at a time. A save is hundreds of those, and the card's file is written once a save: a quiet
// period after the last write, and at shutdown if it still lacks one. The last two checks
// write a page through the EXI registers in a child process that leaves by plat_exit_now,
// as the runtime does, or by a SIGHUP taken on the interrupt thread, and find the page in
// the file.
//
// Stubbed: logging, guest memory, the clock, the event queue and the interrupt line.
//
// usage: test_exi <work dir>
// The work dir must not exist. It's made, and the files are left in it. Exits 0 only if
// every check passes.
#include "hw/exi.cpp"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
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

// The defaults with a flash ID in each slot, as the CARD library leaves them: 12 bytes at
// 0x14 and at 0x20, none of them zero, and at 0x3A and 0x3B the complement of each one's sum.
void with_flash_ids(uint8_t* s) {
    sram_defaults(s);
    for (int card = 0; card < 2; card++) {
        uint8_t sum = 0;
        for (int i = 0; i < 12; i++) {
            s[0x14 + 12 * card + i] = (uint8_t)(0x10 + 0x30 * card + 3 * i);
            sum += s[0x14 + 12 * card + i];
        }
        s[0x3A + card] = (uint8_t)~sum;
    }
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

// The command word for the SRAM from offset `at`, whose address counts a byte as 64.
uint32_t at_offset(uint32_t command, int at) { return command + ((uint32_t)at << 6); }

// Bytes `at` to the end of the SRAM `s`, after the command word.
void send_span(const uint8_t* s, int at) {
    send(at_offset(kWriteSram, at), 4);
    for (int i = at; i < SRAM_SIZE; i += 4)
        send((uint32_t)s[i] << 24 | (uint32_t)s[i + 1] << 16 | (uint32_t)s[i + 2] << 8 | s[i + 3], 4);
}

// The SRAM's 64 bytes from offset 0, where the OS starts its sync of the first 0x14 bytes.
// It writes the rest, the flash IDs, from offset 0x14 (later_offsets, below).
void send_sram(const uint8_t* s) { send_span(s, 0); }

void guest_write(const uint8_t* s) {
    select_ipl();
    send_sram(s);
    deselect();
}

// Bytes `at` to the end of the SRAM, read into the same place in `s`, in one selection.
void read_span(uint8_t* s, int at) {
    select_ipl();
    send(at_offset(kReadSram, at), 4);
    for (int i = at; i < SRAM_SIZE; i += 4) {
        const uint32_t w = receive(4);
        for (int j = 0; j < 4; j++) s[i + j] = (uint8_t)(w >> (24 - 8 * j));
    }
    deselect();
}

void guest_read(uint8_t* s) { read_span(s, 0); }

// A boot: the paths the frontend sets, then exi_init. The save dir is always one in the work
// dir, and the checks run from the work dir, so a file saved to a path that's ignored, the
// save dir's or exi_init's fallback relative to the working directory, lands in the work dir.
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

// The OS writes the flash IDs, from 0x14 to the end, in a span of their own, and reads them
// back the same way.
void later_offsets() {
    uint8_t want[SRAM_SIZE];
    with_flash_ids(want);
    check(sram_checksums_ok(want), "the SRAM with flash IDs has the right checksums");

    const std::string p = path_of("flash.raw");
    boot(p, nullptr);
    select_ipl();
    send_span(want, 0x14);
    deselect();
    check(read_file(p) == bytes(want), "a span written from offset 0x14 reaches the SRAM and is saved");

    const std::string q = path_of("flash_read.raw");
    sram_save(q, want);
    boot(q, nullptr);
    uint8_t got[SRAM_SIZE] = {};
    read_span(got, 0x14);
    check(!memcmp(got + 0x14, want + 0x14, SRAM_SIZE - 0x14), "a span read from offset 0x14 reads the flash IDs");
}

// ---- the memory card ----
using Clock = std::chrono::steady_clock;
constexpr std::chrono::milliseconds kQuiet{400};
constexpr uint32_t kPageSize = 128;
constexpr uint32_t kFirstPage = 0x10000;  // past the card's header, directory and block table
constexpr uint8_t kPageProgram = 0xF2;
constexpr uint32_t kSelectCard = 1u << 7;  // chip select 0, the card in slot A

uint32_t page_addr(int page) { return kFirstPage + (uint32_t)page * kPageSize; }
uint8_t page_byte(int page, uint32_t i) { return (uint8_t)(page * 7 + i + 1); }

// The page program's command byte and address bytes, as the card takes them.
void page_header(uint32_t a, uint8_t* h) {
    h[0] = kPageProgram;
    h[1] = (uint8_t)(a >> 17 & 3);
    h[2] = (uint8_t)(a >> 9);
    h[3] = (uint8_t)(a >> 7 & 3);
    h[4] = (uint8_t)(a & 0x7F);
}

// One page program in one selection of the card, as the CARD library writes a save.
void program_page(MemCard& card, int page) {
    uint8_t h[5];
    page_header(page_addr(page), h);
    card.select();
    for (uint8_t b : h) card.transfer(b, false);
    for (uint32_t i = 0; i < kPageSize; i++) card.transfer(page_byte(page, i), false);
    card.deselect();
}

// Whether the file at `path` holds pages `first` to `first + count - 1`.
bool file_has_pages(const std::string& path, int first, int count) {
    const std::string f = read_file(path);
    for (int page = first; page < first + count; page++) {
        if (f.size() < page_addr(page) + kPageSize) return false;
        for (uint32_t i = 0; i < kPageSize; i++)
            if ((uint8_t)f[page_addr(page) + i] != page_byte(page, i)) return false;
    }
    return true;
}

// When the card's file was seen written more than `count` times, polled for up to 10 s, or
// Clock::time_point::max() if it wasn't.
Clock::time_point wait_for_save(const MemCard& card, uint64_t count) {
    const Clock::time_point give_up = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < give_up) {
        if (card.saves() > count) return Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Clock::time_point::max();
}

void card_saves_after_quiet() {
    const std::string p = path_of("quiet.raw");
    MemCard card(p, 4, kQuiet);
    const uint64_t made = card.saves();  // the new card, formatted
    const int pages = 64;
    Clock::time_point last;
    for (int page = 0; page < pages; page++) {
        last = Clock::now();
        program_page(card, page);
    }
    check(card.saves() == made, "64 page writes in a row don't write the card's file");
    const Clock::time_point saved = wait_for_save(card, made);
    check(saved != Clock::time_point::max(), "they're written once they stop");
    check(saved - last >= kQuiet, "a quiet period after the last of them, not before");
    std::this_thread::sleep_for(2 * kQuiet);
    check(card.saves() == made + 1, "in one write of the file");
    check(file_has_pages(p, 0, pages), "which holds every page");
    card.shutdown();
    check(card.saves() == made + 1, "shutdown doesn't write a file that has every write");
}

void card_write_puts_save_off() {
    const std::string p = path_of("put_off.raw");
    MemCard card(p, 4, kQuiet);
    const uint64_t made = card.saves();
    program_page(card, 0);
    std::this_thread::sleep_for(kQuiet / 4);
    const Clock::time_point second = Clock::now();
    program_page(card, 1);
    const Clock::time_point saved = wait_for_save(card, made);
    check(saved != Clock::time_point::max() && saved - second >= kQuiet,
          "a write within the quiet period puts the file's write off a quiet period from it");
}

void card_shutdown_saves() {
    const std::string p = path_of("shutdown.raw");
    MemCard card(p, 4, kQuiet);
    const uint64_t made = card.saves();
    for (int page = 0; page < 16; page++) program_page(card, page);
    card.shutdown();
    check(card.saves() == made + 1 && file_has_pages(p, 0, 16), "shutdown writes the pages the file lacks, once");
    program_page(card, 16);
    check(card.saves() == made + 2 && file_has_pages(p, 0, 17), "a page written after shutdown is written as it comes");
}

void card_failed_save_is_tried_again() {
    const std::string p = path_of("card_retry.raw");
    MemCard card(p, 4, kQuiet);
    const uint64_t made = card.saves();
    mkdir((p + ".tmp").c_str(), 0755);
    program_page(card, 0);
    std::this_thread::sleep_for(2 * kQuiet);
    rmdir((p + ".tmp").c_str());
    wait_for_save(card, made);
    check(file_has_pages(p, 0, 1), "a card write that fails is tried again a quiet period on");
}

// A child process that starts as the frontend does, writes a page through the EXI registers
// and then calls `leave`. Returns its wait status. `handlers` installs the crash handlers
// first, with an interrupt exiting 0.
int child_writes_page(const std::string& path, bool handlers, void (*leave)()) {
    g_card_a.reset();  // so no other thread is running at the fork
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        alarm(10);  // SIGALRM ends a child that never leaves
        if (handlers)
            plat_install_crash_handlers([] { plat_exit_now(0); }, [](const void*, int) { plat_exit_now(1); });
        g_memcard_path = path.c_str();
        exi_init();
        uint8_t h[5];
        page_header(page_addr(0), h);
        exi_write32(kCsr, kSelectCard);
        send((uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3], 4);
        send((uint32_t)h[4] << 24, 1);
        for (uint32_t i = 0; i < kPageSize; i += 4)
            send((uint32_t)page_byte(0, i) << 24 | (uint32_t)page_byte(0, i + 1) << 16 |
                 (uint32_t)page_byte(0, i + 2) << 8 | page_byte(0, i + 3), 4);
        deselect();
        leave();
        _exit(3);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

// The runtime leaves through plat_exit_now, which runs no destructors, well within a quiet
// period of a save's last page.
void card_saved_at_exit() {
    const std::string p = path_of("exit.raw");
    const int status = child_writes_page(p, false, [] { plat_exit_now(0); });
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && file_has_pages(p, 0, 1),
          "a page written just before plat_exit_now is in the card's file");
}

void card_saved_at_hangup() {
    const std::string p = path_of("hangup.raw");
    const int status = child_writes_page(p, true, [] {
        kill(getpid(), SIGHUP);
        for (;;) pause();
    });
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && file_has_pages(p, 0, 1),
          "and one written just before a SIGHUP, which the interrupt thread exits on");
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
    if (chdir(g_dir.c_str()) != 0) {
        perror(g_dir.c_str());
        return 2;
    }
    g_dir = ".";
    read_only();
    changed_span();
    unchanged_span();
    next_boot_reads_it();
    path_choice();
    failed_save_is_tried_again();
    later_offsets();
    card_saves_after_quiet();
    card_write_puts_save_off();
    card_shutdown_saves();
    card_failed_save_is_tried_again();
    card_saved_at_exit();
    card_saved_at_hangup();
    printf("test_exi: %d of %d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
