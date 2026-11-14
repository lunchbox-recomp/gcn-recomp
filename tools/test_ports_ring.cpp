// ---- The focus ring over a window from SDL's dummy video driver, on a host clock moved by hand
// (g_host_ns): it goes around the item with the focus, as the page laid that item out, in the
// accent and focus_gap outside it. A move a row down eases it, 1 - exp(-dt / focus_ms) of the way
// a frame later, and once it's there nothing moves and the paused wait is 16 ms again. A move
// over half the panel, a page change from the footer and the menu opening put it there at once,
// as menu_motion off and --dump-output do for every move. Closed after all that, 600 frames lay
// out no menu frame. Read in by tools/test_ports.cpp after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"
#include "overlay/ring.h"
#include "overlay/tabs.h"  // how far the page's contents are across as they come in
#include <cfloat>
#include <cmath>

namespace {

constexpr int kFrameMs = 16;  // a host frame over the held game, once nothing moves (batch_wait_ms)

// The Rows page: One and Two a row apart, then Far under a gap of 45% of the window's height,
// over half the panel's. Each one's rect as it was last laid out, and its presses.
ImRect g_rows[3];
int g_pressed[3];
void rows_page(void*) {
    const char* const kNames[] = {"One", "Two", "Far"};
    for (int i = 0; i < 3; i++) {
        if (i == 2) ImGui::Dummy(ImVec2(1.0f, 0.45f * ImGui::GetMainViewport()->Size.y));
        if (ImGui::Button(kNames[i])) g_pressed[i]++;
        g_rows[i] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
}

// The Corner pages, each a button at the page's right, so near the footer's Quit game that a move
// from one to the other isn't over half the panel. Its rect as it was last laid out.
ImRect g_corner;
void corner_page(void*) {
    const char* const label = "Corner";
    const float w = ImGui::CalcTextSize(label).x + 2.0f * ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
    ImGui::Button(label);
    g_corner = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// The ring at the menu's last frame.
struct Ring {
    bool shown = false;
    ImRect at, item;
};
Ring ring_now() {
    float a[4] = {}, b[4] = {};
    Ring r;
    r.shown = ring_test(a, b);
    r.at = ImRect(a[0], a[1], a[2], a[3]);
    r.item = ImRect(b[0], b[1], b[2], b[3]);
    return r;
}

bool same_rect(const ImRect& a, const ImRect& b, float within = 0.01f) {
    return std::fabs(a.Min.x - b.Min.x) <= within && std::fabs(a.Min.y - b.Min.y) <= within &&
           std::fabs(a.Max.x - b.Max.x) <= within && std::fabs(a.Max.y - b.Max.y) <= within;
}

// Showing, on `item`, and on the item with the focus.
bool ring_on(const Ring& r, const ImRect& item) { return r.shown && same_rect(r.at, item) && same_rect(r.item, item); }

// A host frame `ms` of host time after the last.
void frame_after(Guest& g, int ms = kFrameMs) {
    g_host_ns += (uint64_t)ms * kMs;
    host_frame(g);
}

// A key pressed then let go, a host frame each, `down_ms` and `up_ms` after the last.
void stroke(Guest& g, SDL_Scancode sc, SDL_Keycode kc, int down_ms = kFrameMs, int up_ms = kFrameMs) {
    push_key(sc, kc, true);
    frame_after(g, down_ms);
    push_key(sc, kc, false);
    frame_after(g, up_ms);
}

// Long enough for anything moving to have settled.
void ring_settle(Guest& g) { frame_after(g, 1000); }

// The menu set up with Right, Right too and Rows after its own pages, `motion` on or off, under
// --dump-output or not, then opened over the held game, on the page titled `open_on` if one is
// given, as the first open of a session.
void ring_menu(SDL_Window* window, const std::string& dir, Guest& guest, bool motion = true, bool dump_output = false,
               const char* open_on = nullptr) {
    overlay_test_reset_pages();
    overlay_add_page("Right", corner_page, nullptr);
    overlay_add_page("Right too", corner_page, nullptr);
    overlay_add_page("Rows", rows_page, nullptr);
    OverlayPicture picture;
    picture.path = dir + "/ring-settings.txt";
    picture.apply = record_apply;
    picture.now.menu_motion = motion;
    gx::g_dump_output = dump_output;
    overlay_init(window, "Test title", picture);
    gx::g_dump_output = false;
    if (open_on) overlay_test_pick_page(page_at(open_on));
    host_frame(guest);
    ring_settle(guest);
    stroke(guest, SDL_SCANCODE_F1, SDLK_F1);
    ring_settle(guest);
}

// E pressed until the page titled `title` shows: a key's change, which shows the ring.
void turn_to(Guest& g, const char* title) {
    for (int i = 0; i < 8 && overlay_page() != page_at(title); i++) stroke(g, SDL_SCANCODE_E, SDLK_E);
}

// With `motion` off or under --dump-output: a row down puts the ring there at the next frame,
// and nothing moves.
bool still_ring(SDL_Window* window, const std::string& dir, bool motion, bool dump_output) {
    Guest guest;
    ring_menu(window, dir, guest, motion, dump_output);
    turn_to(guest, "Rows");
    ring_settle(guest);
    const bool first = ring_on(ring_now(), g_rows[0]);
    stroke(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);
    const bool there = ring_on(ring_now(), g_rows[1]) && !overlay_animating();
    stroke(guest, SDL_SCANCODE_F1, SDLK_F1);
    ring_settle(guest);
    return first && there;
}

// The rect around the vertices of the foreground's draw list in the colour `rgb` (0xRRGGBB), any
// alpha, at the menu's last frame. Inverted with none.
ImRect drawn_in(uint32_t rgb) {
    ImRect r(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    const ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 want = theme_colour(rgb << 8 | 0xFF) & ~IM_COL32_A_MASK;
    for (const ImDrawVert& v : dl->VtxBuffer)
        if ((v.col & ~IM_COL32_A_MASK) == want) r.Add(v.pos);
    return r;
}

// A left click at the middle of page `title`'s pill, a host frame each for its press and its let go.
void click_pill(Guest& g, const char* title) {
    float x = 0.0f, y = 0.0f;
    tabs_test_pill(page_at(title), x, y);
    mouse(g, x, y, true);
    mouse(g, x, y, false);
}

// The menu shows the ring as it opens: at a session's first open on Rows it's on One, which a
// gamepad's A presses at once, and the same once the mouse has hidden it and the menu has closed.
void ring_opens(SDL_Window* window, const std::string& dir) {
    gamepads_init();
    Virtual pad{"virtual pad ring"};
    attach(pad);
    {
        Guest guest;
        ring_menu(window, dir, guest, true, false, "Rows");
        g_pressed[0] = 0;
        const bool first = overlay_page() == page_at("Rows") && ring_on(ring_now(), g_rows[0]);
        tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
        check(first && g_pressed[0] == 1,
              "at the session's first open the ring is on the page's first item, and A presses it at once");

        // A click on a pill turns the page and hides the ring, and one on Rows' puts the focus back
        // on One with the ring still hidden.
        click_pill(guest, "Right");
        click_pill(guest, "Rows");
        ring_settle(guest);
        const bool hidden = overlay_page() == page_at("Rows") && !ring_now().shown;
        stroke(guest, SDL_SCANCODE_F1, SDLK_F1);
        ring_settle(guest);
        stroke(guest, SDL_SCANCODE_F1, SDLK_F1);
        ring_settle(guest);
        const bool again = ring_on(ring_now(), g_rows[0]);
        tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
        check(hidden && again && g_pressed[0] == 2,
              "after a click hid it, the menu opened again shows the ring on the first item, and A presses it at once");
        stroke(guest, SDL_SCANCODE_F1, SDLK_F1);
        ring_settle(guest);
    }
    unplug({&pad});
    gamepads_shutdown();
}

void ring_moves(const std::string& dir) {
    SDL_Window* window = open_window();
    if (!window) return;
    const OverlayTheme theme = overlay_theme_default();
    {
        Guest guest;
        ring_menu(window, dir, guest);
        const long frames_open = overlay_menu_frames();

        // A page change from the footer's Quit game to a page whose first item is under half the
        // panel from it, so that only the change puts the ring there at once.
        turn_to(guest, "Right");
        ring_settle(guest);
        const bool corner = ring_on(ring_now(), g_corner);
        stroke(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);
        ring_settle(guest);
        const Ring footer = ring_now();
        const bool in_footer = footer.shown && footer.item.Min.y > g_corner.Max.y && same_rect(footer.at, footer.item);
        stroke(guest, SDL_SCANCODE_E, SDLK_E);
        check(corner && in_footer && overlay_page() == page_at("Right too") && ring_on(ring_now(), g_corner),
              "the ring goes around the item with the focus, and a page change from the footer puts it on the "
              "new page's first item at once");

        // Drawn in the accent, focus_gap outside the item and focus_width wide at the window's scale,
        // and moved across with the page's contents as they come in.
        const float s = ImGui::GetStyle().FontScaleMain;
        const float outside = std::round(theme.focus_gap * s) + std::max(1.0f, std::round(theme.focus_width * s));
        ImRect expect = g_corner;
        expect.Expand(outside);
        float dx = 0.0f, alpha = 1.0f;
        tabs_contents_moved(g_host_ns, dx, alpha);
        ImRect slid = expect;
        slid.Translate(ImVec2(dx, 0.0f));
        const bool sliding = dx > 1.0f && same_rect(drawn_in(theme.accent >> 8), slid, 1.0f);
        ring_settle(guest);
        check(sliding, "as a page change slides the page's contents in, the ring slides in with them");
        const ImRect drawn = drawn_in(theme.accent >> 8);
        check(same_rect(drawn, expect, 1.0f) && ImGui::GetStyle().Colors[ImGuiCol_NavCursor].w == 0.0f,
              "it's drawn in the accent, focus_gap outside its item and focus_width wide, and Dear ImGui's own "
              "highlight is clear");

        turn_to(guest, "Rows");
        ring_settle(guest);
        const bool one = ring_on(ring_now(), g_rows[0]) && !overlay_animating();
        stroke(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);
        const Ring part = ring_now();
        const float k = 1.0f - std::exp(-(float)kFrameMs / theme.focus_ms);
        const float y = g_rows[0].Min.y + k * (g_rows[1].Min.y - g_rows[0].Min.y);
        const bool fast = batch_wait_ms() == 8;
        check(one && part.shown && same_rect(part.item, g_rows[1]) && std::fabs(part.at.Min.y - y) < 0.05f &&
                  part.at.Min.y < g_rows[1].Min.y && fast,
              "a row down, a frame later the ring is 1 - exp(-dt / focus_ms) of the way there, the wait 8 ms");
        int frames = 1;
        for (; frames < 60 && overlay_animating(); frames++) frame_after(guest);
        // 0.5 px left of a row's move, at 16 ms frames.
        const int expect_frames = (int)std::ceil(std::log(0.5 / (g_rows[1].Min.y - g_rows[0].Min.y)) /
                                                 std::log(1.0 - k));
        check(ring_on(ring_now(), g_rows[1]) && !overlay_animating() && batch_wait_ms() == 16 &&
                  std::abs(frames - expect_frames) <= 1,
              "it settles on the item within half a pixel, then nothing moves and the wait is 16 ms again");

        stroke(guest, SDL_SCANCODE_DOWN, SDLK_DOWN);
        const bool far_down = ring_on(ring_now(), g_rows[2]) && !overlay_animating();
        stroke(guest, SDL_SCANCODE_UP, SDLK_UP);
        check(far_down && ring_on(ring_now(), g_rows[1]), "a move over half the panel puts the ring there at once, either way");

        // A row up with no host time passing leaves it where it was, and the menu closed and opened
        // again with none passing puts it there.
        stroke(guest, SDL_SCANCODE_UP, SDLK_UP, kFrameMs, 0);
        const Ring stood = ring_now();
        stroke(guest, SDL_SCANCODE_F1, SDLK_F1, 0, 0);
        stroke(guest, SDL_SCANCODE_F1, SDLK_F1, 0, 0);
        check(stood.shown && same_rect(stood.item, g_rows[0]) && same_rect(stood.at, g_rows[1]) && overlay_open() &&
                  ring_on(ring_now(), g_rows[0]),
              "the menu opening puts the ring on its item at once");

        stroke(guest, SDL_SCANCODE_F1, SDLK_F1);
        ring_settle(guest);
        const long gone = overlay_menu_frames();
        for (int i = 0; i < 600; i++) host_frame(guest);
        check(!overlay_open() && overlay_menu_frames() == gone && gone > frames_open,
              "closed after the ring has moved, 600 frames lay out no menu frame");
    }
    check(still_ring(window, dir, false, false), "with menu_motion off the ring is on its item at the next frame");
    ring_opens(window, dir);
    check(still_ring(window, dir, true, true), "under --dump-output it doesn't move either");
    close_window(window);
}

}  // namespace
