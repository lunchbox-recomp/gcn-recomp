// External Interface: IPL device (SRAM / RTC / UART). Memory cards: not present (yet).
#include "../runtime.h"
#include "../platform.h"
#include <ctime>
#include <memory>
#include <mutex>
#include "memcard.h"
#include "sram.h"

static std::unique_ptr<MemCard> g_card_a;

struct ExiChannel {
    uint32_t csr, mar, len, cr, data;
};
static ExiChannel g_exi[3];

// ---- IPL device (channel 0, CS1) ----
// The SRAM lives in a file (sram.h), read at boot and written each time the guest changes
// it. g_sram_saved is what the file holds, or the defaults when there is none, so a write
// that changes nothing writes no file: a run that never changes the SRAM leaves none behind.
static uint8_t g_sram[SRAM_SIZE];
static uint8_t g_sram_saved[SRAM_SIZE];
static std::string g_sram_file;
static bool g_sram_written;
static uint32_t g_ipl_addr;
static bool g_ipl_have_cmd;
static uint32_t g_ipl_pos;
static std::string g_uart_line;

// The guest writes the SRAM a span at a time, each in one selection of the device, with
// its checksums already worked out, so it's saved when the device is deselected.
static void sram_commit() {
    g_sram_written = false;
    if (!memcmp(g_sram, g_sram_saved, SRAM_SIZE)) return;
    if (!sram_save(g_sram_file, g_sram)) return;
    memcpy(g_sram_saved, g_sram, SRAM_SIZE);
    LOG(LOG_EXI, "sram: saved %s", g_sram_file.c_str());
}

// The RTC runs on guest time from a base the frontend sets (the input log's start time
// on a replay, the wall clock otherwise), so a replay sees the recording's calendar.
static time_t g_rtc_base;
void rtc_set_base(time_t utc) { g_rtc_base = utc; }

static uint32_t rtc_now() {
    // seconds since 2000-01-01 00:00:00 local time
    if (!g_rtc_base) g_rtc_base = time(nullptr);
    time_t t = g_rtc_base + (time_t)(now_ticks() / TB_FREQ);
    time_t local = t + plat_utc_offset_seconds();
    return (uint32_t)(local - 946684800);
}

static uint8_t ipl_transfer_byte(uint8_t in) {
    if (!g_ipl_have_cmd) {
        g_ipl_addr = (g_ipl_addr << 8) | in;
        if (++g_ipl_pos == 4) { g_ipl_have_cmd = true; g_ipl_pos = 0; }
        return 0;
    }
    bool write = g_ipl_addr & 0x80000000u;
    uint32_t addr = g_ipl_addr & 0x7FFFFFFFu;
    uint8_t out = 0;
    // The command's address counts a byte as 64, its low six bits unused. The RTC's four bytes
    // are at 0x20000000 and the SRAM's 64 follow from 0x20000100, and a span may start at any
    // of them: the OS writes the flash IDs from SRAM offset 0x14.
    if ((addr & 0x7FFFFF00u) == 0x20000000u) {  // RTC
        uint32_t rtc = rtc_now();
        if (!write && g_ipl_pos < 4) out = (uint8_t)(rtc >> (24 - 8 * g_ipl_pos));
    } else if (addr >= 0x20000100u && addr < 0x20000100u + ((uint32_t)SRAM_SIZE << 6)) {  // SRAM
        uint32_t o = ((addr - 0x20000100u) >> 6) + g_ipl_pos;
        if (o < 64) {
            if (write) {
                g_sram[o] = in;
                g_sram_written = true;
            } else {
                out = g_sram[o];
            }
        }
    } else if ((addr & 0x7FFFFF00u) == 0x20010000u) {  // UART
        if (write && g_ipl_pos > 0) {  // first byte after cmd is a length/pad byte on some SDKs
            if (in == '\n' || in == '\r') {
                if (!g_uart_line.empty()) LOG(LOG_OS, "UART: %s", g_uart_line.c_str());
                g_uart_line.clear();
            } else if (in) g_uart_line.push_back((char)in);
        }
    } else {
        // ROM (font etc.) - not provided
        if (g_ipl_pos == 0) LOG(LOG_EXI, "IPL ROM read at %08X (unsupported)", addr >> 6);
    }
    g_ipl_pos++;
    return out;
}

static void ipl_deselect() {
    g_ipl_have_cmd = false;
    g_ipl_pos = 0;
    g_ipl_addr = 0;
    if (g_sram_written) sram_commit();
}

// ---- generic ----
static int selected_device(int ch) {
    uint32_t cs = (g_exi[ch].csr >> 7) & 7;
    if (cs & 1) return 0;
    if (cs & 2) return 1;
    if (cs & 4) return 2;
    return -1;
}

static uint8_t device_byte(int ch, int dev, uint8_t in, bool reading) {
    if (ch == 0 && dev == 1) return ipl_transfer_byte(in);
    if (ch == 0 && dev == 0 && g_card_a) return g_card_a->transfer(in, reading);
    return 0xFF;
}

static void exi_update_irq() {
    bool irq = false;
    for (auto& c : g_exi) {
        if ((c.csr & 0x2) && (c.csr & 0x1)) irq = true;
        if ((c.csr & 0x8) && (c.csr & 0x4)) irq = true;
        if ((c.csr & 0x800) && (c.csr & 0x400)) irq = true;
    }
    pi_set_interrupt(INT_EXI, irq);
}

static void exi_transfer(int ch) {
    ExiChannel& c = g_exi[ch];
    int dev = selected_device(ch);
    uint32_t rw = (c.cr >> 2) & 3;
    if (c.cr & 2) {  // DMA
        uint8_t* p = phys_ptr(c.mar);
        for (uint32_t i = 0; i < c.len; i++) {
            uint8_t o = device_byte(ch, dev, rw == 1 ? p[i] : 0, rw != 1);
            if (rw == 0) p[i] = o;
        }
        c.mar += c.len;
        c.len = 0;
    } else {
        uint32_t n = ((c.cr >> 4) & 3) + 1;
        uint32_t result = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t in = (uint8_t)(c.data >> (24 - 8 * i));
            uint8_t o = device_byte(ch, dev, rw != 0 ? in : 0, rw != 1);
            result |= (uint32_t)o << (24 - 8 * i);
        }
        if (rw != 1) c.data = result;
    }
    c.cr &= ~1u;
    c.csr |= 0x8;  // TCINT
    exi_update_irq();
}

const char* g_save_dir;
// Set before boot to use a specific card file instead of <save dir>/memcard_a.raw. A replay
// points this at its scratch copy of the logged run's card.
const char* g_memcard_path;
// Set before boot to use a specific SRAM file instead of <save dir>/sram.raw. A replay
// points this at its scratch copy of the logged run's.
const char* g_sram_path;

// The card writes its file on a thread of its own (memcard.h), and the runtime leaves through
// plat_exit_now, which skips destructors, so the card is shut down from there. An exit can
// come on another thread while exi_init replaces the card, so the two hold g_card_mutex.
static std::mutex g_card_mutex;

static void exi_shutdown() {
    std::lock_guard<std::mutex> lk(g_card_mutex);
    if (g_card_a) g_card_a->shutdown();
}

void exi_init() {
    static bool s_at_exit = false;
    if (!s_at_exit) {
        s_at_exit = true;
        plat_at_exit(exi_shutdown);
    }
    g_sram_file = g_sram_path ? std::string(g_sram_path)
                : g_save_dir  ? std::string(g_save_dir) + "/sram.raw"
                              : std::string("saves/sram.raw");
    if (sram_load(g_sram_file, g_sram)) LOG(LOG_EXI, "sram: loaded %s", g_sram_file.c_str());
    memcpy(g_sram_saved, g_sram, SRAM_SIZE);
    g_sram_written = false;
    const std::string path = g_memcard_path ? std::string(g_memcard_path)
                           : g_save_dir ? std::string(g_save_dir) + "/memcard_a.raw"
                                        : std::string("saves/memcard_a.raw");
    std::lock_guard<std::mutex> lk(g_card_mutex);
    g_card_a.reset();  // the old card saves what it hasn't before the new one reads the file
    g_card_a = std::make_unique<MemCard>(path, 4);
}

bool exi_card_unsaved() {
    std::lock_guard<std::mutex> lk(g_card_mutex);
    return g_card_a && g_card_a->unsaved();
}

uint32_t exi_read32(uint32_t off) {
    int ch = off / 0x14;
    if (ch > 2) return 0;
    ExiChannel& c = g_exi[ch];
    switch (off % 0x14) {
    case 0x00: {
        uint32_t v = c.csr & ~0x1000u;
        if (ch == 0 && g_card_a) v |= 0x1000;  // EXT: memory card inserted in slot A
        // EXT: device attached. Channel 0/1 memory cards: absent. Channel 2 (AD16): absent.
        return v;
    }
    case 0x04: return c.mar;
    case 0x08: return c.len;
    case 0x0C: return c.cr;
    case 0x10: return c.data;
    }
    return 0;
}

void exi_write32(uint32_t off, uint32_t v) {
    int ch = off / 0x14;
    if (ch > 2) return;
    ExiChannel& c = g_exi[ch];
    switch (off % 0x14) {
    case 0x00: {
        int old_dev = selected_device(ch);
        uint32_t clr = v & (0x2 | 0x8 | 0x800);
        uint32_t keep = c.csr & (0x2 | 0x8 | 0x800 | 0x2000);
        keep &= ~clr;
        c.csr = (v & ~(0x2u | 0x8u | 0x800u | 0x1000u)) | keep;
        int new_dev = selected_device(ch);
        if (old_dev != new_dev && ch == 0 && old_dev == 1) ipl_deselect();
        if (new_dev == 1 && ch == 0 && old_dev != 1) ipl_deselect();
        if (ch == 0 && g_card_a && old_dev != new_dev) {
            if (old_dev == 0 && g_card_a->deselect()) {
                // Card signals completion of erase/program via its EXI interrupt.
                event_schedule_in(TB_FREQ / 2000, [] { g_exi[0].csr |= 0x2; exi_update_irq(); });
            }
            if (new_dev == 0) g_card_a->select();
        }
        exi_update_irq();
        break;
    }
    case 0x04: c.mar = v & 0x03FFFFE0; break;
    case 0x08: c.len = v & 0x03FFFFE0; break;
    case 0x0C:
        if (!(ch == 0 && selected_device(ch) == 1))
            LOG(LOG_EXI, "EXI ch%d dev%d xfer cr=%02X data=%08X len=%X csr=%08X", ch, selected_device(ch), v & 0x3F, c.data, c.len, c.csr);
        c.cr = v & 0x3F;
        if (v & 1) exi_transfer(ch);
        break;
    case 0x10: c.data = v; break;
    }
}
