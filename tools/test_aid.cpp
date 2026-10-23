// AI DMA checks: the `test_aid` target. Runs with no device, disc or game.
//
// runtime/hw/dsp.cpp is compiled into this file over a fake clock, event queue and main
// memory. The guest is a double buffer, as a game's mixer is: when it's free to take the
// interrupt a block's start raises, it acknowledges it and queues the other buffer. What the
// DMA hands the device is recorded by address, and a buffer played twice in a row is a repeat.
//
// A guest busy through a whole block gets its buffer played again, as the hardware plays it.
// A catch-up jump that lands inside a block while the guest is busy must not do the same: the
// block is held for up to a quarter block of the guest running, with the jumps left out, and
// ends soon after the guest takes the interrupt and queues its next buffer. A guest that
// stays busy through that quarter block gets the repeat after it, so the wait is bounded.
//
// usage: test_aid
// Exits 0 only if every check passes.
#include "hw/dsp.cpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

uint8_t* g_mem;
uint8_t* g_aram;
bool g_log_enabled[LOG_COUNT];
void log_msg(LogCat, const char*, ...) {}
void dsp_hle_reset() {}
void dsp_hle_mail(uint32_t) {}
void dsp_hle_cpu_interrupt() {}
uint32_t dma_fit(const char*, uint32_t, uint32_t len) { return len; }
void debug_guard_dma(const char*, uint32_t, uint32_t) {}
void debug_guest_check(const char*) {}
uint32_t ai_dma_sample_rate() { return 32000; }

namespace {

constexpr uint32_t kBufA = 0x00100000, kBufB = 0x00100400;
constexpr uint16_t kBlocks = 20;  // 32-byte blocks: 160 frames, 5 ms
constexpr uint64_t kDur = 160 * TB_FREQ / 32000;

struct Event {
    uint64_t at, seq;
    EventFn fn;
};
struct Played {
    uint32_t addr;
    uint64_t at;
};

uint64_t g_now, g_jumped, g_seq;
std::vector<Event> g_events;
std::vector<Played> g_played;
bool g_irq, g_busy;
uint32_t g_next;
int g_failed;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_failed++;
}

void queue(uint32_t addr) {
    dsp_write16(0x30, (uint16_t)(addr >> 16));
    dsp_write16(0x32, (uint16_t)addr);
}

// The guest's AI callback, run whenever it's free and the DSP interrupt is up.
void guest_poll() {
    if (g_busy || !g_irq) return;
    dsp_write16(0x0A, CR_AID | CR_AID_MASK | 0x800);  // acknowledge, keeping the mask and DSPInit
    queue(g_next);
    g_next = g_next == kBufA ? kBufB : kBufA;
}

// Runs the events due by `t` in time order, the guest polling after each, and leaves the
// clock at `t`.
void run_events_to(uint64_t t) {
    for (;;) {
        size_t best = g_events.size();
        for (size_t i = 0; i < g_events.size(); i++) {
            const Event& e = g_events[i];
            if (e.at <= t && (best == g_events.size() || e.at < g_events[best].at ||
                              (e.at == g_events[best].at && e.seq < g_events[best].seq)))
                best = i;
        }
        if (best == g_events.size()) break;
        Event e = std::move(g_events[best]);
        g_events.erase(g_events.begin() + (std::ptrdiff_t)best);
        g_now = std::max(g_now, e.at);
        e.fn();
        guest_poll();
    }
    g_now = std::max(g_now, t);
    guest_poll();
}

// The guest runs on to `t`.
void run_to(uint64_t t) {
    guest_poll();
    run_events_to(t);
}

// A catch-up jump: the clock moves on with the guest not running, and at the poll it lands
// on the events due run before the guest takes an interrupt, as irq_poll runs them.
void jump(uint64_t ticks) {
    g_now += ticks;
    g_jumped += ticks;
    run_events_to(g_now);
}

// When the block in flight started: the last block played ended there.
uint64_t block_start() { return g_played.back().at; }

int repeats(size_t from) {
    int n = 0;
    for (size_t i = std::max<size_t>(from, 1); i < g_played.size(); i++) n += g_played[i].addr == g_played[i - 1].addr;
    return n;
}

}  // namespace

uint64_t now_ticks() { return g_now; }
uint64_t clock_jumped_ticks() { return g_jumped; }
void event_schedule(uint64_t at, EventFn fn) { g_events.push_back(Event{at, g_seq++, std::move(fn)}); }
void event_schedule_in(uint64_t delta, EventFn fn) { event_schedule(g_now + delta, std::move(fn)); }
void pi_set_interrupt(uint32_t cause, bool set) {
    if (cause == INT_DSP) g_irq = set;
}
void audio_push_dma(const int16_t* samples, uint32_t) {
    g_played.push_back({(uint32_t)((const uint8_t*)samples - g_mem), g_now});
}

int main() {
    g_mem = (uint8_t*)calloc(0x02000000, 1);
    g_aram = (uint8_t*)calloc(ARAM_SIZE, 1);
    dsp_write16(0x0A, CR_AID_MASK | 0x800);
    queue(kBufA);
    g_next = kBufB;
    dsp_write16(0x36, 0x8000 | kBlocks);  // starts on A, and the guest queues B at once
    run_to(20 * kDur + 1);
    check(g_played.size() == 20 && repeats(0) == 0, "a guest that answers at once hears its buffers in turn");

    // Busy from before a block starts until after it ends: nothing new was queued.
    size_t from = g_played.size();
    const uint64_t start1 = block_start();
    run_to(start1 + kDur / 2);
    g_busy = true;
    run_to(start1 + 5 * kDur / 2);
    g_busy = false;
    run_to(g_now + 10 * kDur);
    check(repeats(from) == 1, "a guest busy through a whole block hears the buffer again, as the hardware plays it");

    // Busy from before a block starts, and 1 ms into it the clock jumps past its end.
    from = g_played.size();
    run_to(block_start() + kDur / 2);
    g_busy = true;
    run_to(block_start() + kDur + kDur / 5);
    const uint64_t start = block_start();
    g_busy = false;
    jump(6 * kDur / 5);
    run_to(g_now + 10 * kDur);
    printf("  a jump of 6 ms 1 ms into a block: %d repeat(s) after it\n", repeats(from));
    check(repeats(from) == 0, "a jump inside a block leaves the guest time to queue the next buffer");
    // The jump lands 7/5 of a block after the block started, the block is held there and the
    // guest queues at once; the hold is looked at again a 32nd of a block later.
    check(g_played[from + 1].at == start + 7 * kDur / 5 + kDur / 32,
          "the held block ends at the first look after the guest queues, not a quarter block on");

    // The same, with the guest still busy for a quarter block after the jump.
    from = g_played.size();
    run_to(block_start() + kDur / 2);
    g_busy = true;
    run_to(block_start() + kDur + kDur / 5);
    const uint64_t start2 = block_start();
    jump(6 * kDur / 5);
    run_to(g_now + kDur / 4);
    g_busy = false;
    run_to(g_now + 10 * kDur);
    check(g_played[from + 1].at == start2 + 6 * kDur / 5 + kDur / 4 && repeats(from) == 1,
          "a guest still busy after its quarter block hears the buffer again then");

    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
