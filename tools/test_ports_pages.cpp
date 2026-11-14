// ---- The menu's review dumps over a hidden window with a GL context, at 16:9 and at 4:3:
// GCN_OVERLAY_PAGE opens it on each page in turn, and --dump-output writes each page once its
// frame has settled, over GCN_OVERLAY_PLAIN's grey, since no game frame is kept here. Each
// page's menu frame time, overlay_draw's on the CPU, is printed. The PNGs are for a person to
// look at, never goldens. With a GL window here, it also checks that overlay_draw's frames end
// a view the menu closed from once the fade is over. With no GL window, as on a machine with no
// display, the mode is skipped (exit 77). Read in by tools/test_ports.cpp after the frontend,
// with GCN_OVERLAY.
#include "test_ports_support.h"
#include "overlay/fonts.h"  // the atlas the menu and the notices share
#include <cstdio>

namespace {

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
// its own, not on its grab (theme.h).
void sample_page(void*) {
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
    double first_ms = -1;    // overlay_draw on the CPU as it first drew, making its GL objects
    std::vector<double> ms;  // the same for each frame after that but the dump's, the backdrop's alone too
};

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
    overlay_init(window, "Test title", picture);
    unsetenv("GCN_OVERLAY_PAGE");
    unsetenv("GCN_OVERLAY_PLAIN");
    unsetenv("GCN_OVERLAY_AT");
    const std::string png = out + "/overlay_page" + std::to_string(expect) + ".png";
    PageRun run;
    Guest guest;
    for (int f = 0; f < held + 5; f++) {
        pump_events();
        update_pads(0);
        const bool was_open = overlay_open() || overlay_menu_due();
        const int atlas_frame = fonts_atlas()->Builder->FrameCount;
        const auto t0 = std::chrono::steady_clock::now();
        overlay_draw();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // A new atlas's count starts from fonts_new_frame's, which rises across atlases.
        const int counted = fonts_atlas()->Builder->FrameCount;
        run.atlas_each_draw = run.atlas_each_draw && (f == 0 ? counted > atlas_frame : counted == atlas_frame + 1);
        SDL_GL_SwapWindow(window);
        const bool any = was_open && ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 0;
        const bool drew = any && menu_drawn();
        const bool dumping = run.dumped_at < 0 && exists_path(png);
        if (any && run.first_ms < 0) run.first_ms = ms;
        else if (any && !dumping) run.ms.push_back(ms);
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
        for (int page = 0; page < 3; page++) {
            const std::string said = out + "/page" + std::to_string(page) + "-stderr.txt";
            PageRun run;
            capture(said, [&] { run = open_on(window, std::to_string(page).c_str(), page, true, out); });
            each_draw = each_draw && run.atlas_each_draw;
            const Png png = png_at(out + "/overlay_page" + std::to_string(page) + ".png");
            const std::string at = std::to_string(pw) + "x" + std::to_string(ph) + " " + kTitles[page];
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
            printf("[pages] %s: menu frame first %.3f ms, then p50 %.3f ms, p99 %.3f ms, max %.3f ms over %zu frames\n",
                   at.c_str(), run.first_ms, percentile(run.ms, 0.5), percentile(run.ms, 0.99), percentile(run.ms, 1.0),
                   run.ms.size());
        }
        check(each_draw, (std::to_string(pw) + "x" + std::to_string(ph) +
                          ": the shared atlas has its own update once at each overlay_draw, which no context makes")
                             .c_str());
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
        }
        gl_window_close(window);
    }
    return finish("pages");
}

}  // namespace
