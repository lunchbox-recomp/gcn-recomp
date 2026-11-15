// ---- The menu's review dumps over a hidden window with a GL context, at 16:9 and at 4:3:
// GCN_OVERLAY_PAGE opens it on each page in turn, and --dump-output writes each page once its
// frame has settled, over GCN_OVERLAY_PLAIN's grey, since no game frame is kept here. Each
// page's menu frame time, overlay_draw's on the CPU, is printed. At 16:9 GCN_FRAMETIME is set, and
// each page's `[overlay] frame` lines are checked against the host clock, which the test steps by
// known amounts, against whether the guest was held, and against the test's own timing of overlay_draw,
// and each page's first menu frame, the process's first too, is held to 1.5 ms of the calling
// thread's CPU time where CI is unset, so time off the CPU doesn't count, and printed without
// failing where it's set; at 4:3 it's unset, and there are none.
// After every page, the Buttons
// view is dumped at each size too, as it opens and at its foot. The PNGs are for a person to
// look at, never goldens. With a GL window here, it also checks that overlay_draw's frames end
// a view the menu closed from once the fade is over, that a reopen after a close starts its frame
// lines with no interval, and that overlay_init's warm-up draw leaves
// the GL state a game's renderer had as it found it. With no GL window, as on a machine with no
// display, the mode is skipped (exit 77). Read in by tools/test_ports.cpp after the frontend,
// with GCN_OVERLAY.
#include "test_ports_support.h"
#include "overlay/fonts.h"  // the atlas the menu and the notices share
#include "overlay/footer.h"  // where the footer drew its hints
#include "overlay/review.h"  // the Buttons view's dumps
#include <cmath>
#include <cstdio>
#include <ctime>

namespace {

// The calling thread's CPU time in ms. Time the process spends off the CPU doesn't add to it.
double thread_cpu_ms() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

constexpr int kSkipped = 77;  // ctest's SKIP_RETURN_CODE for this mode
constexpr int kHeldFrames = 40;  // GCN_OVERLAY_AT's host frames for each page
const char* const kTitles[] = {"Display", "Controllers", "Sample"};
constexpr int kPlainGrey = 128;  // GCN_OVERLAY_PLAIN's background, each channel

// A channel of the theme's backdrop, `shift` bits up in its 0xRRGGBBAA, laid over the plain grey.
int over_grey(uint32_t backdrop, int shift) {
    const int a = (int)(backdrop & 0xFF), c = (int)(backdrop >> shift & 0xFF);
    return (kPlainGrey * (255 - a) + c * a + 127) / 255;
}

// Whether the menu's window, not just the backdrop behind it, is in the frame drawn.
bool menu_drawn() {
    const ImDrawData* d = ImGui::GetDrawData();
    const ImGuiWindow* w = ImGui::FindWindowByName("Test title");
    return d && w && d->CmdLists.contains(w->DrawList);
}

// A page as a game might add one, with the kinds of widget the menu's own pages use and a
// slider, so the dumps show each kind. The slider's value is beside it, as the Buttons view has
// its own, not on its grab (theme.h). It moves the host clock on by 0.5 ms as it's drawn, inside
// overlay_draw, so a line's interval timed from the end of the frame before shows it short.
void sample_page(void*) {
    g_host_ns += 500000;
    static bool on = true;
    static int pick = 1;
    static float level = 0.6f;
    ImGui::SeparatorText("Options");
    ImGui::Checkbox("A switch", &on);
    ImGui::RadioButton("First", &pick, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Second", &pick, 1);
    ImGui::SliderFloat("##Level", &level, 0.0f, 1.0f, "");
    ImGui::SameLine();
    ImGui::Text("Level %.2f", level);
    ImGui::TextDisabled("A dimmed note.");
    ImGui::Button("A button");
}

// A hidden window of `w` by `h` pixels, whatever the display's pixel density, with a GL context
// current. Null, having said why, if SDL can't make one.
SDL_Window* gl_window(int w, int h) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        printf("no video: %s\n", SDL_GetError());
        return nullptr;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, GCN_GL_MAJOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, GCN_GL_MINOR);
    SDL_Window* window =
        SDL_CreateWindow("test_ports pages", w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    if (density > 1.0f) SDL_SetWindowSize(window, (int)(w / density), (int)(h / density));
    SDL_GLContext ctx = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!ctx) {
        printf("no GL window: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        return nullptr;
    }
    SDL_GL_MakeCurrent(window, ctx);
#ifndef __APPLE__
    gl_load_with((GLLoadProc)SDL_GL_GetProcAddress);
#endif
    return window;
}

void gl_window_close(SDL_Window* window) {
    SDL_GL_DestroyContext(SDL_GL_GetCurrentContext());
    SDL_DestroyWindow(window);
}

// What png.cpp wrote at `path`: IHDR's width and height, and the first pixel, which follows the
// IDAT's length and type, the zlib header, the first stored block's header and the first row's
// filter byte. All 0 for no file.
struct Png {
    int w = 0, h = 0;
    uint8_t first[3] = {};
};
Png png_at(const std::string& path) {
    Png p;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return p;
    uint8_t b[53] = {};
    if (fread(b, 1, sizeof(b), f) == sizeof(b)) {
        p.w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
        p.h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
        memcpy(p.first, b + 49, 3);
    }
    fclose(f);
    return p;
}

struct PageRun {
    int first_drawn = -1;    // the first frame that drew the menu's window
    int dumped_at = -1;      // the frame whose draw wrote the dump
    bool on_page = false;    // the page asked for was selected as the menu first drew
    bool ended = false;      // its Dear ImGui contexts were destroyed once it was done
    bool atlas_made = false;  // the shared font atlas had its GL texture before they were (fonts.h)
    bool notices_on_atlas = false;  // the notices' context was on it and last drew with its texture
    bool atlas_each_draw = true;  // the atlas's own update came once at each overlay_draw (fonts_new_frame)
    bool let_go = false;     // Display, picked after the dump, was still selected near the end
    double init_ms = 0;      // overlay_init on the CPU, which makes the GL renderers' objects
    double first_ms = -1;    // overlay_draw on the CPU as it first drew, making the atlas's texture
    double first_thread_ms = -1;  // the same on the calling thread's CPU clock, which the bar holds
    std::vector<double> ms;  // the same for each frame after that but the dump's, the backdrop's alone too
    // Each overlay_draw that laid out the menu: its frame, the host clock as it started, whether
    // the guest was held then, and its time on the CPU.
    struct Drawn {
        int f;
        uint64_t host;
        bool held;
        double ms;
    };
    std::vector<Drawn> drawn;
};

// The host clock's step before each frame of open_on, in turn, so the line's intervals are known.
constexpr uint64_t kHostSteps[] = {1000000, 2500000, 16670000};

// An `[overlay] frame` line (GCN_FRAMETIME).
struct FrameLine {
    long f = 0;
    int held = -1;
    double interval = 0, cpu = 0;
    double late = 0, present = 0, blur = 0;  // what the frontend did before it (overlay_frame_pre)
};
std::vector<FrameLine> frame_lines(const std::string& said) {
    std::vector<FrameLine> out;
    const std::string text = read_file(said);
    for (size_t at = 0; (at = text.find("[overlay] frame f", at)) != std::string::npos; at++) {
        FrameLine l;
        if (sscanf(text.c_str() + at, "[overlay] frame f%ld held %d interval %lfms cpu %lfms late %lfms present %lfms blur %lfms",
                   &l.f, &l.held, &l.interval, &l.cpu, &l.late, &l.present, &l.blur) == 7)
            out.push_back(l);
    }
    return out;
}

// "Smooth while animating"'s bar for the menu's CPU time a frame at 1920x1080, which the first
// frame of an open is held to too: overlay_init has made the GL renderers' objects (overlay.cpp).
// The first frame's is read on the thread's CPU clock. macOS's GL compiles the program on the
// calling thread, so a first frame without the warm-up still reads 12 ms or more there.
constexpr double kFrameBarMs = 1.5;

// Whether `lines` are one for each frame `run` drew the menu in, numbered on from the first's,
// each saying whether the guest was held as it started, with frames of both, the first with no
// interval and each after it the host clock's steps since the one before to the printed 0.01 ms,
// and each one's cpu above 0 and no more than the test timed around its overlay_draw.
bool lines_match(const std::vector<FrameLine>& lines, const PageRun& run) {
    bool ok = !lines.empty() && lines.size() == run.drawn.size() && lines[0].interval == 0.0;
    bool held = false, running = false;
    for (size_t i = 0; ok && i < lines.size(); i++) {
        const PageRun::Drawn& d = run.drawn[i];
        ok = lines[i].f == lines[0].f + (long)i && lines[i].held == (d.held ? 1 : 0) && lines[i].cpu > 0.0 &&
             lines[i].cpu <= d.ms + 0.001;
        if (ok && i > 0) ok = std::abs(lines[i].interval - (double)(d.host - run.drawn[i - 1].host) / 1e6) < 0.006;
        held = held || d.held;
        running = running || !d.held;
    }
    return ok && held && running;
}

// Whether the notices' context is on the atlas the menu's is made on, and each draw command of
// its last frame takes that atlas's texture.
bool notices_on_atlas() {
    ImGuiContext* menu = ImGui::GetCurrentContext();
    ImGuiContext* notices = overlay_test_notices_context();
    if (!notices) return false;
    ImGui::SetCurrentContext(notices);
    const ImDrawData* d = ImGui::GetDrawData();
    const ImTextureID atlas_tex = fonts_atlas()->TexData->TexID;
    bool on = ImGui::GetIO().Fonts == fonts_atlas() && atlas_tex != ImTextureID_Invalid && d && d->CmdListsCount > 0;
    for (int i = 0; on && i < d->CmdListsCount; i++)
        for (const ImDrawCmd& cmd : d->CmdLists[i]->CmdBuffer) on = on && cmd.GetTexID() == atlas_tex;
    ImGui::SetCurrentContext(menu);
    return on;
}

// The menu opened by GCN_OVERLAY_AT=0:<held> with GCN_OVERLAY_PAGE=`page`, over the plain
// background or not, dumping to `out`, for as long as it stays open and a few frames more.
// `expect` is the page it should open on.
PageRun open_on(SDL_Window* window, const char* page, int expect, bool plain, const std::string& out, int held = kHeldFrames) {
    static std::string dump_dir;
    dump_dir = out;
    gx::g_dump_dir = dump_dir.c_str();
    setenv("GCN_OVERLAY_PAGE", page, 1);
    if (plain) setenv("GCN_OVERLAY_PLAIN", "1", 1);
    setenv("GCN_OVERLAY_AT", ("0:" + std::to_string(held)).c_str(), 1);
    overlay_test_reset_pages();
    overlay_add_page("Sample", sample_page, nullptr);
    OverlayPicture picture;
    picture.path = out + "/settings.txt";
    picture.apply = record_apply;
    PageRun run;
    const auto init_at = std::chrono::steady_clock::now();
    overlay_init(window, "Test title", picture);
    run.init_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - init_at).count();
    unsetenv("GCN_OVERLAY_PAGE");
    unsetenv("GCN_OVERLAY_PLAIN");
    unsetenv("GCN_OVERLAY_AT");
    const std::string png = out + "/overlay_page" + std::to_string(expect) + ".png";
    Guest guest;
    for (int f = 0; f < held + 5; f++) {
        pump_events();
        update_pads(0);
        const bool was_open = overlay_open() || overlay_menu_due();
        const int atlas_frame = fonts_atlas()->Builder->FrameCount;
        g_host_ns += kHostSteps[f % std::size(kHostSteps)];
        const uint64_t host = g_host_ns;
        const bool held_then = paused();
        const auto t0 = std::chrono::steady_clock::now();
        const double c0 = thread_cpu_ms();
        overlay_draw();
        const double thread_ms = thread_cpu_ms() - c0;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // A new atlas's count starts from fonts_new_frame's, which rises across atlases.
        const int counted = fonts_atlas()->Builder->FrameCount;
        run.atlas_each_draw = run.atlas_each_draw && (f == 0 ? counted > atlas_frame : counted == atlas_frame + 1);
        SDL_GL_SwapWindow(window);
        const bool any = was_open && ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 0;
        const bool drew = any && menu_drawn();
        const bool dumping = run.dumped_at < 0 && exists_path(png);
        if (any && run.first_ms < 0) {
            run.first_ms = ms;
            run.first_thread_ms = thread_ms;
        } else if (any && !dumping) run.ms.push_back(ms);
        if (was_open) run.drawn.push_back({f, host, held_then, ms});
        if (drew && run.first_drawn < 0) {
            run.first_drawn = f;
            run.on_page = overlay_page() == expect && overlay_test_pages()[expect] == kTitles[expect];
        }
        if (dumping) run.dumped_at = f;
        // A frame after the dump Display is picked, as a click would pick it, and the page the menu
        // opened on doesn't take it back.
        if (run.dumped_at >= 0 && f == run.dumped_at + 1) overlay_test_pick_page(0);
        if (f == held - 2) run.let_go = overlay_page() == 0;
        guest_on(guest);
    }
    const ImTextureData* tex = fonts_atlas()->TexData;
    run.atlas_made = tex->Status == ImTextureStatus_OK && tex->TexID != ImTextureID_Invalid;
    run.notices_on_atlas = notices_on_atlas();
    overlay_test_end();
    run.ended = !ImGui::GetCurrentContext() && !overlay_test_notices_context();
    return run;
}

// The press-A order, up as F1 closes the menu, ends once the fade is over at a frame of
// overlay_draw, the game's, as it does at one of overlay_menu_frame (test_ports_motion.cpp).
bool order_ends_at_draw(SDL_Window* window, const std::string& out) {
    g_window_id = SDL_GetWindowID(window);
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = out + "/order-settings.txt";
    gx::g_dump_output = false;  // dumping turns the menu's motion off, and with it the fade
    overlay_init(window, "Test title", picture);
    Guest guest;
    const auto frame = [&] {
        pump_events();
        update_pads(0);
        overlay_draw();
        SDL_GL_SwapWindow(window);
        guest_on(guest);
    };
    const auto f1 = [&] {
        push_key(SDL_SCANCODE_F1, SDLK_F1, true);
        push_key(SDL_SCANCODE_F1, SDLK_F1, false);
        frame();
    };
    f1();
    g_host_ns += 1000000000;  // past the open's motion
    frame();
    overlay_controllers_order();
    frame();
    const bool asked = overlay_open() && overlay_controllers_asking() == 0;
    f1();
    const bool kept = !overlay_open() && overlay_controllers_asking() == 0;
    g_host_ns += 1000000000;  // past the close's fade
    frame();
    const bool ended = overlay_controllers_asking() == -1;
    overlay_test_end();
    gx::g_dump_output = true;
    return asked && kept && ended;
}

// The menu opened with F1, closed, drawn closed past its fade, and opened again, all after one
// overlay_init with GCN_FRAMETIME set and the host clock 16 ms on at each frame. Whether the
// reopen's first `[overlay] frame` line has no interval, though its last line before was over a
// second earlier on that clock, and the fade's frames before it printed lines. Each open's first
// frame, drawn before the guest's hold has landed, says held 0, the next held 1, and the fade's
// frames, with the guest let go, held 0.
bool reopen_starts_fresh(SDL_Window* window, const std::string& out) {
    g_window_id = SDL_GetWindowID(window);
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = out + "/reopen-settings.txt";
    gx::g_dump_output = false;  // dumping turns the menu's motion off, and with it the fade
    const std::string said = out + "/reopen-stderr.txt";
    long opened = -1, closed = -1, reopened = -1;
    capture(said, [&] {
        setenv("GCN_FRAMETIME", "1", 1);
        overlay_init(window, "Test title", picture);
        unsetenv("GCN_FRAMETIME");
        Guest guest;
        const auto frame = [&] {
            pump_events();
            update_pads(0);
            // A close only wakes the held guest, whose thread lets the hold go, so the frame waits
            // for that, bounded as guest_on is, and its line's held is known. An open's hold lands
            // only at the boundary guest_on lets it reach, after the draw.
            for (int i = 0; i < 2000 && paused() && !pause_requested(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            g_host_ns += 16000000;
            overlay_draw();
            SDL_GL_SwapWindow(window);
            guest_on(guest);
        };
        const auto f1 = [&] {
            push_key(SDL_SCANCODE_F1, SDLK_F1, true);
            push_key(SDL_SCANCODE_F1, SDLK_F1, false);
            frame();
        };
        opened = overlay_menu_frames();
        f1();
        for (int i = 0; i < 4; i++) frame();
        closed = overlay_menu_frames();
        f1();
        for (int i = 0; i < 70; i++) frame();  // past the fade, the menu drawn no more
        reopened = overlay_menu_frames();
        f1();
        frame();
        overlay_close();
    });
    overlay_test_end();
    gx::g_dump_output = true;
    const std::vector<FrameLine> lines = frame_lines(said);
    bool fresh = false, faded = false, held = true;
    int seen = 0;
    for (const FrameLine& l : lines) {
        fresh = fresh || (l.f == reopened && l.interval == 0.0);
        faded = faded || l.f == reopened - 1;
        const int want = l.f == opened || l.f == reopened || (l.f >= closed && l.f < reopened) ? 0 : 1;
        held = held && l.held == want;
        seen += l.f == opened || l.f == opened + 1 || l.f == closed;
    }
    return reopened > 0 && fresh && faded && held && seen == 3 && lines.size() < 70;
}

// The GL state the warm-up's draw at overlay_init (overlay.cpp) must leave as it found it.
struct GlState {
    GLint draw = 0, read = 0, program = 0, active = 0, viewport[4] = {}, scissor[4] = {};
    GLint tex0 = 0, tex_active = 0;  // the 2D texture bound on unit 0 and on the active unit
    GLint unpack = 0;                // the pixel unpack buffer
    GLboolean blend = GL_FALSE;
    bool operator==(const GlState&) const = default;
};
GlState gl_state() {
    GlState s;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.read);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.active);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex_active);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s.unpack);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex0);
    glActiveTexture((GLenum)s.active);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s.scissor);
    s.blend = glIsEnabled(GL_BLEND);
    return s;
}

// overlay_init over a GL state as a game's renderer might leave it: render_init leaves the EFB's
// framebuffer bound, so here the draw and read framebuffers are two of the test's own, apart, and
// the viewport, scissor box, blend and active texture unit are away from what the overlay's
// renderer sets, with a texture of the test's own bound on unit 0 and another on the active unit,
// and a buffer of its own as the pixel unpack buffer, as a game's texture uploads might leave one.
// Whether all of it, and the program, is as it was after.
bool init_keeps_gl_state(SDL_Window* window, const std::string& out) {
    GLuint fbos[2], texs[2], unpack = 0;
    glGenFramebuffers(2, fbos);
    glGenTextures(2, texs);
    glGenBuffers(1, &unpack);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[0]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[1]);
    glViewport(3, 4, 50, 60);
    glScissor(5, 6, 70, 80);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texs[0]);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, texs[1]);
    const GlState before = gl_state();
    overlay_test_reset_pages();
    OverlayPicture picture;
    picture.path = out + "/state-settings.txt";
    capture(out + "/state-stderr.txt", [&] { overlay_init(window, "Test title", picture); });
    const GlState after = gl_state();
    overlay_test_end();
    glDeleteTextures(2, texs);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(1, &unpack);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fbos);
    return before.draw == (GLint)fbos[0] && before.read == (GLint)fbos[1] && before.tex0 == (GLint)texs[0] &&
           before.tex_active == (GLint)texs[1] && before.unpack == (GLint)unpack && after == before;
}

// tools/test_ports_pages_toasts.cpp's: the notices' review dump at a size, and their cost with the
// menu closed.
void toasts_review(SDL_Window* window, const std::string& out, const std::string& at);

// Whether each of the Buttons view's rows, as last drawn, was a gamepad of family `f`'s and has
// its name whole left of its button, two item spacings short of it, and its button whole across
// the region the rows scroll in, inside its padding. The gap may be short by float rounding, a
// hundredth of a point at most.
bool rows_whole(PadFamily f) {
    const std::vector<OverlayBindingsRow>& rows = overlay_bindings_test_rows();
    bool whole = rows.size() == kPadControls;
    for (const OverlayBindingsRow& r : rows)
        whole = whole && r.family == f && r.region[0] <= r.name[0] &&
                r.name[1] + 2.0f * r.spacing <= r.button[0] + 0.01f && r.button[1] <= r.region[1];
    return whole;
}

// The Buttons view at a size, over a gamepad plugged in for it, in place of the Controllers
// page: dumped to buttons/overlay_buttons_top.png as it opens with its first gamepad focused, and
// to buttons/overlay_buttons_end.png once the D-pad has taken the focus down to Done, its last
// item. At each the footer's hints and the item with the focus are whole on the panel
// (on_panel), and where the panel, the hints and the last row are is printed. Then on it and on
// a pad of each family, every row is whole across (rows_whole).
void buttons_review(SDL_Window* window, const std::string& out, const std::string& at) {
    static std::string dump_dir;
    dump_dir = out + "/buttons";
    plat_make_dirs(dump_dir);
    Virtual pad{"virtual pad buttons review"};
    attach(pad);
    frame();
    setenv("GCN_OVERLAY_PAGE", "1", 1);
    setenv("GCN_OVERLAY_AT", "0:1000", 1);  // open at the first draw, and closed below
    overlay_test_reset_pages();
    overlay_add_page("Sample", sample_page, nullptr);
    OverlayPicture picture;
    picture.path = dump_dir + "/settings.txt";
    picture.apply = record_apply;
    gx::g_dump_dir = nullptr;  // no dump of the menu's own as it settles, only the two below
    overlay_init(window, "Test title", picture);
    unsetenv("GCN_OVERLAY_PAGE");
    unsetenv("GCN_OVERLAY_AT");
    overlay_bindings_open(pad.id);
    Guest guest;
    const auto draw = [&] {
        pump_events();
        update_pads(0);
        overlay_draw();
        SDL_GL_SwapWindow(window);
        guest_on(guest);
    };
    const auto dump = [&](const char* name, const char* where) {
        gx::g_dump_dir = dump_dir.c_str();
        review_write(window, ImGui::GetDrawData(), name, true);
        gx::g_dump_dir = nullptr;
        const ImGuiWindow* panel = ImGui::FindWindowByName("Test title");
        float hints[4], last[4];
        footer_test_hints_rect(hints);
        overlay_bindings_test_last_row(last);
        printf("[pages] %s Buttons view, %s: panel y %.0f to %.0f, footer hints y %.0f to %.0f, last row y %.0f to %.0f\n",
               at.c_str(), where, panel ? panel->Pos.y : 0.0f, panel ? panel->Pos.y + panel->Size.y : 0.0f, hints[1],
               hints[3], last[1], last[3]);
        return exists_path(dump_dir + "/" + name);
    };
    for (int f = 0; f < 6; f++) draw();
    const bool top = dump("overlay_buttons_top.png", "as it opens") && on_panel() && focused(first_pad());
    for (int i = 0; i < 40; i++) {
        set_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        draw();
        set_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        draw();
    }
    const bool end = dump("overlay_buttons_end.png", "at Done") && on_panel() && focused("Done");
    // The view on that pad and on one of each family whose buttons are named their own way, each
    // row's name whole left of its button, and its button whole across the region. A Joy-Con
    // alone with a trigger axis is held upright, and a Steam Deck has misc1 (pad_glyph.h). The
    // GameCube pad has its control stick's x turned round, so that row's button says "Control
    // stick (reversed)", the widest a stick's says, and is wider than the y's "Control stick".
    bool whole = rows_whole(PadFamily::other);
    bool reversed = false;
    const struct {
        const char* name;
        Uint16 vendor, product;
        PadFamily family;
    } kFamilies[] = {
        {"xbox pad buttons review", 0x045e, 0x02ea, PadFamily::xbox},
        {"playstation pad buttons review", 0x054c, 0x0ce6, PadFamily::ps5},
        {"gc pad buttons review", 0x057e, 0x0337, PadFamily::gamecube},
        {"joy-con pad buttons review", 0x057e, 0x2006, PadFamily::joycon_left_upright},
        {"steam deck pad buttons review", 0x28de, 0x1205, PadFamily::steam},
    };
    for (const auto& f : kFamilies) {
        Virtual v{f.name};
        v.vendor = f.vendor;
        v.product = f.product;
        attach(v);
        frame();
        PadBinding b;
        const bool cube = f.family == PadFamily::gamecube;
        if (cube && gamepads_binding(v.id, b)) {
            b.source[(int)PadControl::StickX].invert = !b.source[(int)PadControl::StickX].invert;
            whole = whole && gamepads_bind(v.id, b) != Bound::refused;
        }
        whole = whole && overlay_bindings_open(v.id);
        draw();
        draw();
        whole = whole && rows_whole(f.family);
        if (cube) {
            const std::vector<OverlayBindingsRow>& rows = overlay_bindings_test_rows();
            const auto wide = [&](PadControl c) { return rows[(int)c].button[1] - rows[(int)c].button[0]; };
            reversed = rows.size() == kPadControls && wide(PadControl::StickX) > wide(PadControl::StickY);
            gamepads_reset_binding(v.id);
        }
        detach(v);
        frame();
    }
    whole = whole && reversed;
    check(whole, (at + ": each of the Buttons view's rows has its name whole left of its button, and its button "
                       "whole across the view, on a generic, Xbox, PlayStation, GameCube, Joy-Con and Steam Deck pad, "
                       "the GameCube one with a reversed stick")
                     .c_str());
    overlay_close();
    overlay_test_end();
    detach(pad);
    frame();
    check(top && end, (at + ": the Buttons view is dumped as it opens and at Done, the item with the focus and the "
                            "footer's hints whole on the panel at each")
                          .c_str());
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
}

int run_pages(const std::string& dir) {
    if (!sdl_up()) return 1;
    no_controls(dir);
    power_on(1);
    gamepads_init();
    gx::g_dump_output = true;
    static const struct {
        int w, h;
    } kSizes[] = {{1920, 1080}, {1440, 1080}};
    for (const auto& size : kSizes) {
        SDL_Window* window = gl_window(size.w, size.h);
        if (!window) {
            printf("test_ports pages: skipped\n");
            return kSkipped;
        }
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        const std::string out = dir + "/" + std::to_string(pw) + "x" + std::to_string(ph);
        plat_make_dirs(out);
        bool each_draw = true;
        // GCN_FRAMETIME at the first size, read as each open_on inits the overlay, and not at the second.
        const bool timed = size.w == kSizes[0].w;
        bool first_in_bar = true;
        for (int page = 0; page < 3; page++) {
            const std::string said = out + "/page" + std::to_string(page) + "-stderr.txt";
            PageRun run;
            if (timed) setenv("GCN_FRAMETIME", "1", 1);
            capture(said, [&] { run = open_on(window, std::to_string(page).c_str(), page, true, out); });
            unsetenv("GCN_FRAMETIME");
            each_draw = each_draw && run.atlas_each_draw;
            const Png png = png_at(out + "/overlay_page" + std::to_string(page) + ".png");
            const std::string at = std::to_string(pw) + "x" + std::to_string(ph) + " " + kTitles[page];
            const std::vector<FrameLine> lines = frame_lines(said);
            if (timed) {
                check(lines_match(lines, run),
                      (at + ": GCN_FRAMETIME=1 prints an [overlay] frame line for each frame overlay_draw lays out "
                            "the menu in, with whether the guest was held, the host clock's interval from the last "
                            "one's start and its CPU time")
                          .c_str());
                first_in_bar = first_in_bar && run.first_thread_ms >= 0 && run.first_thread_ms <= kFrameBarMs;
                std::vector<double> cpu, interval;
                for (size_t i = 0; i < lines.size() && i < run.drawn.size(); i++) {
                    if (run.drawn[i].f == run.dumped_at) continue;  // a game draws no dump
                    cpu.push_back(lines[i].cpu);
                    if (i > 0 && lines[i].held == 1) interval.push_back(lines[i].interval);
                }
                printf("[pages] %s: [overlay] frame lines past the dump's, cpu p50 %.3f ms, p99 %.3f ms; held interval "
                       "p99 %.2f ms, on the host clock this test steps; over %zu frames\n",
                       at.c_str(), percentile(cpu, 0.5), percentile(cpu, 0.99), percentile(interval, 0.99), cpu.size());
            } else {
                check(lines.empty() && !run.drawn.empty(),
                      (at + ": with GCN_FRAMETIME unset, the menu's frames print no [overlay] frame line").c_str());
            }
            const std::string opens = "GCN_OVERLAY_PAGE=" + std::to_string(page) + " opens the menu on " + at;
            check(run.on_page && run.let_go && run.ended,
                  (opens + ", another can be picked, and its contexts are destroyed after").c_str());
            const uint32_t dim = overlay_theme_default().backdrop;
            bool grey = true;
            for (int i = 0; i < 3; i++) grey = grey && std::abs(png.first[i] - over_grey(dim, 24 - 8 * i)) <= 1;
            check(png.w == pw && png.h == ph && grey,
                  (at + " is dumped at the window's size, its backdrop over GCN_OVERLAY_PLAIN's grey").c_str());
            check(run.first_drawn >= 0 && run.dumped_at > run.first_drawn + 1 && lines_with(said, "settled") == 0,
                  (at + " is dumped once its frame has settled, not at its first drawn frame").c_str());
            printf("[pages] %s: overlay_init %.3f ms; menu frame first %.3f ms (thread CPU %.3f ms), then p50 %.3f ms, "
                   "p99 %.3f ms, max %.3f ms over %zu frames\n",
                   at.c_str(), run.init_ms, run.first_ms, run.first_thread_ms, percentile(run.ms, 0.5), percentile(run.ms, 0.99),
                   percentile(run.ms, 1.0), run.ms.size());
        }
        // A CI runner's VM draws in software, tens of ms a frame, so there the bar is printed, not held.
        if (timed && getenv_nonempty("CI"))
            printf("[pages] %dx%d: first menu frames %s the 1.5 ms bar, not held where CI is set\n", pw, ph,
                   first_in_bar ? "within" : "over");
        else if (timed)
            check(first_in_bar, (std::to_string(pw) + "x" + std::to_string(ph) +
                                 ": each page's first menu frame, the process's first open's too, takes 1.5 ms or "
                                 "less of overlay_draw's CPU on the calling thread")
                                    .c_str());
        check(each_draw, (std::to_string(pw) + "x" + std::to_string(ph) +
                          ": the shared atlas has its own update once at each overlay_draw, which no context makes")
                             .c_str());
        toasts_review(window, out, std::to_string(pw) + "x" + std::to_string(ph));
        if (size.w == kSizes[0].w) {
            // No game frame is kept here, so without the plain background there's nothing to dump.
            const std::string bare = out + "/bare";
            plat_make_dirs(bare);
            open_on(window, "0", 0, false, bare);
            check(!exists_path(bare + "/overlay_page0.png"), "with no held frame and no GCN_OVERLAY_PLAIN, nothing is dumped");
            const std::string brief = out + "/brief", said = dir + "/brief-stderr.txt";
            plat_make_dirs(brief);
            capture(said, [&] { open_on(window, "0", 0, true, brief, 2); });
            check(!exists_path(brief + "/overlay_page0.png") &&
                      lines_with(said, "[overlay] the menu closed before its frame settled, so there's no dump\n") == 1,
                  "a menu closed before its frame settles writes no dump and says so once");
            // A notice over the open menu, its letters not all on the page, drawn by the notices'
            // own context and GL renderer from the atlas the menu's shares (fonts.h).
            const std::string notice = out + "/notice";
            plat_make_dirs(notice);
            const long notices = overlay_notice_frames();
            PageRun shared;
            capture(dir + "/notice-stderr.txt", [&] {
                overlay_pads_notice("A notice", " over the menu: \xC3\xA0 \xC3\xA9 \xC3\xBC");
                shared = open_on(window, "0", 0, true, notice);
            });
            check(overlay_notice_frames() > notices && exists_path(notice + "/overlay_page0.png") && shared.atlas_made &&
                      shared.notices_on_atlas,
                  "a notice over the open menu draws from the atlas the menu's GL renderer made, both contexts on it");
            const std::string bad = dir + "/bad-page-stderr.txt";
            PageRun past, word;
            capture(bad, [&] {
                past = open_on(window, "3", 0, true, out + "/bare");
                word = open_on(window, "Display", 0, true, out + "/bare");
            });
            check(lines_with(bad, "[overlay] GCN_OVERLAY_PAGE=3 isn't a page from 0 to 2, so it opens where it was\n") == 1 &&
                      lines_with(bad, "[overlay] GCN_OVERLAY_PAGE=Display isn't a page from 0 to 2, so it opens where it was\n") == 1 &&
                      past.on_page && word.on_page,
                  "a GCN_OVERLAY_PAGE past the last page or not a number says so once and opens on the first page");
            check(order_ends_at_draw(window, out + "/bare"),
                  "at overlay_draw's frames, the press-A order up as the menu closes ends once the fade is over");
            check(reopen_starts_fresh(window, out + "/bare"),
                  "with GCN_FRAMETIME, the menu opened again after closing, in one overlay_init, starts its lines "
                  "with no interval, and each line says held 1 only once the guest's hold has landed and until the close");
            check(init_keeps_gl_state(window, out + "/bare"),
                  "overlay_init's warm-up draw leaves the draw and read framebuffers, viewport, scissor box, program, "
                  "blend, active texture unit, the textures bound on unit 0 and on it and the pixel unpack buffer as "
                  "it found them");
        }
        gl_window_close(window);
    }
    // The Buttons view at each size once every page's dump is done, so the gamepad plugged in for
    // it is in none of theirs.
    for (const auto& size : kSizes) {
        SDL_Window* window = gl_window(size.w, size.h);
        if (!window) {
            printf("test_ports pages: skipped\n");
            return kSkipped;
        }
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        const std::string at = std::to_string(pw) + "x" + std::to_string(ph);
        buttons_review(window, dir + "/" + at, at);
        gl_window_close(window);
    }
    return finish("pages");
}

}  // namespace
