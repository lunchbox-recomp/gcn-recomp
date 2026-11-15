// The overlay's set-up, opening and closing, and its frames; see overlay.h. What opens and
// closes it and the input it holds back are overlay_input.cpp, the menu itself overlay_menu.cpp
// with its header tabs.cpp, its footer footer.cpp and its focus ring ring.cpp, the Display page
// overlay_display.cpp, the Controllers page overlay_controllers.cpp and the notices notices.cpp.
#include "overlay/overlay.h"
#include "overlay/appear.h"
#include "overlay/fonts.h"
#include "overlay/footer.h"
#include "overlay/review.h"
#include "overlay/ring.h"
#include "overlay/state.h"
#include "overlay/tabs.h"
#include "gx/gl.h"
#include "gx/render_gl.h"
#include "pause.h"
#include "runtime.h"  // host_ns
#include "ui_sound.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

// overlay_display.cpp's.
void display_init(const OverlayPicture& picture);
// overlay_controllers.cpp's and overlay_bindings.cpp's.
void controllers_stop();
void bindings_stop();
// notices.cpp's.
void notices_init(SDL_Window* window, bool gl);
void notices_motion(bool on);
bool notices_moving(uint64_t now);
void notices_frame();
// overlay_input.cpp's: the gamepads held for the menu, and the buttons each has down kept out of
// the menu until each is let go.
void overlay_hold_pads();
void overlay_latch_pads();
// overlay_menu.cpp's: the menu set up with its title on its first page, how many pages a game
// added, the menu opened on a page (-1 for where it was) at a time, and its frame.
void menu_init(const char* title);
int menu_added_pages();
void menu_opened(int page, uint64_t now);
void menu_frame();
#ifdef GCN_OVERLAY_TEST
void menu_clear_pages();
#endif
// Below, for the notices too: a Dear ImGui context over `window`, made current, with the GL
// backend where `gl` says the window has a GL context and none otherwise, so it's laid out and
// never drawn. It writes no file, opens no link, offers no item picker and reads no gamepad
// itself. The menu and the notices each have one, so a frame of notices with the menu closed
// leaves the menu's focus as it was. Its style is the theme's at the window's height.
void overlay_imgui_context(SDL_Window* window, bool gl);
#ifdef GCN_OVERLAY_TEST
// Below, for the notices too: the current context's backends, GL's where `gl` says it has one,
// shut down and the context destroyed. And notices.cpp's, which does that to theirs.
void overlay_imgui_context_end(bool gl);
void notices_test_end();
#endif
// Below, for the notices too: the current context's style built again from the theme if the
// window's height has changed since it was last built, before a frame.
void overlay_style_fit(SDL_Window* window);

namespace {

// Read by overlay_set_theme and overlay_add_page, which a game may call from a static
// initializer before this file's, so constinit: a dynamic initializer here won't compile.
constinit bool g_ready;
bool g_open;
bool g_view_left;  // it closed from a view in place of a page, drawn as it fades (overlay_set_open)
SDL_Window* g_window;  // null in tests, which draw nothing
bool g_gl;             // the window has a GL context to draw the menu with, which tests' don't
bool g_blur_allowed;    // the theme's blur_backdrop, and not under --dump-output
bool g_dumped;            // since it opened, under --dump-output
long long g_at_frame = -1;  // GCN_OVERLAY_AT's, -1 for none or once it's opened the overlay
long g_at_left;             // draws over the held frame left before GCN_OVERLAY_AT's closes it
int g_page = -1;            // GCN_OVERLAY_PAGE's, 0 for Display, -1 for none
bool g_plain;               // GCN_OVERLAY_PLAIN: dumps over plain grey, not the held frame
ReviewWatch g_watch;        // the menu's frame settling for its dump, from each open
// GCN_FRAMETIME, read at overlay_init: a line for each overlay_draw that lays out the menu, with
// whether the guest is held, the interval on the host clock from the start of the one before, and
// the CPU time this one took (overlay_draw).
bool g_frametime;
bool g_had_frame;       // the draw before laid out the menu
uint64_t g_last_frame;  // and started at this host_ns

// What showed in place of a page ends with nothing done: Restart? or Quit?, the press-A order,
// and the Buttons view with any press-to-bind.
void end_view() {
    g_view_left = false;
    footer_end_ask();
    controllers_stop();
    bindings_stop();
}

// The current context's GL renderer made ready for its first frame, which would otherwise make
// its shader program and buffers as it began and, in macOS's GL, compile the program as it first
// drew with it: 12 to 15 ms of CPU in a process that hadn't compiled it before, where the next
// context's first frame took under 1 ms. So the renderer makes them here and draws once, into the
// window's framebuffer as overlay_draw does, a triangle with no area and no alpha, which touches
// no pixel. It takes no texture and lays out no frame. The renderer puts back the GL state it
// sets but the framebuffer, which this puts back.
void renderer_warm() {
    ImGui_ImplOpenGL3_CreateDeviceObjects();
    ImDrawList list(ImGui::GetDrawListSharedData());
    list._ResetForNewFrame();
    list.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    list.PrimReserve(3, 3);
    for (int i = 0; i < 3; i++) {
        list.PrimWriteVtx(ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f), 0);
        list.PrimWriteIdx((ImDrawIdx)i);
    }
    ImDrawData data;
    data.Valid = true;
    data.DisplaySize = ImVec2(1.0f, 1.0f);
    data.FramebufferScale = ImVec2(1.0f, 1.0f);
    data.AddDrawList(&list);
    GLint draw = 0, read = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_RenderDrawData(&data);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
}

// GCN_OVERLAY_AT's frame has been presented, so overlay_draw opens the menu.
bool at_frame_due() { return g_at_frame >= 0 && gx::present_count() >= g_at_frame; }

// The menu is open or still fading out over a window.
bool menu_showing() { return g_open || (g_window && appear_closing(host_ns())); }

// Whether a frame lays out the menu. Both frames ask it, so a view the menu closed from ends
// once its fade is over (overlay_set_open) on either.
bool menu_lays_out() {
    if (g_view_left && !menu_showing()) end_view();
    return menu_showing();
}

}  // namespace

bool overlay_ready() { return g_ready; }

SDL_Window* overlay_window() { return g_window; }

void overlay_set_open(bool on) {
    if (on == g_open) return;
    g_open = on;
    ui_sound_play(on ? UiSound::open : UiSound::close);
    pause_request(on);
    const uint64_t now = host_ns();
    if (on) appear_open(theme_now(), now);
    else appear_close(theme_now(), now);
    fprintf(stderr, "[overlay] %s\n", on ? "open" : "closed");
    // A view up as it closes is drawn as it was while the menu fades, with no input reaching it,
    // and ends once it's gone (menu_lays_out), or at once with no fade. Opened again before then,
    // the menu is back on its page.
    if (on) {
        if (g_view_left) end_view();
    } else if (g_window && appear_closing(now)) {
        g_view_left = true;
    } else {
        end_view();
    }
    if (!g_window) return;
    if (!on) {
        // As it fades out the menu is laid out with no input fed to it (overlay_event, menu_frame),
        // and with none held, so nothing moves its focus, which Dear ImGui keeps for the next
        // open. A button picked for the focus to go to (actions, asking, controllers_page, the
        // Buttons view's rows) isn't taken there either. Its NoInputs window flag would lose that
        // focus. A key queued ahead of this close in the same pump is dropped too, or the fade's
        // first frame, or with no fade the next open's, would press it. While it's closed no
        // event is fed to it (overlay_event): the SDL backend's NewFrame queues only the mouse's
        // position, where it can read it from the OS, and each fade frame uses up its own.
        ImGui::GetIO().ClearEventsQueue();
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        return;
    }
    // Keys and buttons let go while it was closed never reached the menu. One held as it opens
    // stays out of it until it's let go, though Dear ImGui keeps the focus from the last time,
    // so that it presses nothing: a key's repeats never reach the menu (overlay_event), and a
    // gamepad's buttons are latched here.
    ImGui::GetIO().ClearInputKeys();
    overlay_latch_pads();
    menu_opened(g_page, now);
    g_dumped = false;
    g_watch = {};
}

void overlay_set_theme(const OverlayTheme& theme) {
    if (g_ready) { fprintf(stderr, "[overlay] the theme is fixed after init, so this one is refused\n"); return; }
    theme_set(theme);
}

#ifdef GCN_OVERLAY_TEST
void overlay_test_reset_pages() {
    menu_clear_pages();
    theme_set(overlay_theme_default());
    g_ready = false;
}

void overlay_test_end() {
    if (!g_window) return;
    notices_test_end();
    overlay_imgui_context_end(g_gl);
    g_window = nullptr;
}
#endif

void overlay_style_fit(SDL_Window* window) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    theme_fit(h);
}

void overlay_init(SDL_Window* window, const char* title, const OverlayPicture& picture) {
    g_ready = true;
    g_window = window;
    menu_init(title);
    display_init(picture);
    const bool motion = picture.now.menu_motion && !gx::g_dump_output;
    appear_motion(motion);
    tabs_motion(motion);
    ring_motion(motion);
    notices_motion(motion);
    g_blur_allowed = theme_now().blur_backdrop && !gx::g_dump_output;
    if (const char* e = getenv_nonempty("GCN_OVERLAY_AT")) review_parse_at(e, g_at_frame, g_at_left);
    const char* page = getenv_nonempty("GCN_OVERLAY_PAGE");
    g_page = page ? review_parse_page(page, 1 + menu_added_pages()) : -1;
    g_plain = getenv_nonempty("GCN_OVERLAY_PLAIN") != nullptr;
    g_frametime = getenv_nonempty("GCN_FRAMETIME") != nullptr;
    g_had_frame = false;
    if (!window) return;
    g_gl = SDL_GL_GetCurrentContext() != nullptr;
    fonts_build(theme_now().fonts);  // the atlas both contexts are made on
    notices_init(window, g_gl);
    overlay_imgui_context(window, g_gl);  // the menu's, current from here on
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    overlay_hold_pads();
}

void overlay_imgui_context(SDL_Window* window, bool gl) {
    ImGui::SetCurrentContext(ImGui::CreateContext(fonts_atlas()));
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written to the game's directory
    io.ConfigDebugHighlightIdConflictsShowItemPicker = false;
    io.LogFilename = nullptr;
    io.FontDefault = fonts_role(FontRole::body);
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    theme_style(theme_now(), theme_scale(h));
    if (gl) {
        ImGui_ImplSDL3_InitForOpenGL(window, SDL_GL_GetCurrentContext());
        ImGui_ImplOpenGL3_Init();
        renderer_warm();
    } else {
        // Nothing draws it, so the texture its font is baked into is never made.
        ImGui_ImplSDL3_InitForOther(window);
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }
    // The SDL3 backend sets this to SDL_OpenURL and Dear ImGui's duplicate-ID tooltip
    // carries a link, so clear it after both backend inits.
    ImGui::GetPlatformIO().Platform_OpenInShellFn = nullptr;
    // The backend reads no gamepad, so overlay_read_pads can read them all by their labels.
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
}

#ifdef GCN_OVERLAY_TEST
void overlay_imgui_context_end(bool gl) {
    if (gl) ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}
#endif

bool overlay_open() { return g_open; }
bool overlay_blur_hold() { return g_blur_allowed && g_open && paused(); }
float overlay_blur_alpha() { return g_blur_allowed ? appear_blur(host_ns()) : 0.0f; }

bool overlay_menu_due() { return menu_showing() || at_frame_due(); }

bool overlay_animating() {
    const uint64_t now = host_ns();
    return g_window &&
           (appear_moving(now) || (g_open && (tabs_moving(now) || ring_moving())) || notices_moving(now));
}

void overlay_close() { overlay_set_open(false); }

void overlay_menu_frame() {
    if (!g_window) return;
    fonts_new_frame();
    if (menu_lays_out()) menu_frame();
    notices_frame();
}

namespace {

// overlay_draw's frame, and whether it laid out the menu.
bool draw_frame() {
    fonts_new_frame();  // once a frame, before the menu's and the notices' (fonts.h)
    // Opening ends a view left from the last close itself (overlay_set_open).
    if (at_frame_due()) {
        g_at_frame = -1;
        overlay_set_open(true);
    }
    const bool menu = menu_lays_out();
    if (menu) {
        ImGui_ImplOpenGL3_NewFrame();
        menu_frame();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    notices_frame();
    if (!g_open || !paused()) return menu;
    // The dump waits for a frame that looks as the ones before it did (review.cpp).
    const bool dumps = gx::g_dump_output && gx::g_dump_dir;
    if (!g_dumped && dumps && review_settled(g_watch, ImGui::GetDrawData())) {
        g_dumped = true;
        review_dump(g_window, ImGui::GetDrawData(), g_page, g_plain);
    }
    if (g_at_left > 0 && --g_at_left == 0) {
        if (dumps && !g_dumped) fprintf(stderr, "[overlay] the menu closed before its frame settled, so there's no dump\n");
        overlay_set_open(false);
    }
    return menu;
}

}  // namespace

void overlay_draw() {
    if (!g_window || !g_gl) return;
    if (!g_frametime) {
        draw_frame();
        return;
    }
    // The interval is on host_ns, the clock the menu's motion runs on, which is the wall clock in a
    // game; the CPU time on the steady clock, as the other GCN_FRAMETIME lines take theirs.
    const uint64_t at = host_ns();
    const auto start = std::chrono::steady_clock::now();
    const long frame = overlay_menu_frames();
    const bool held = paused();
    if (!draw_frame()) {
        g_had_frame = false;
        return;
    }
    const double cpu = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // The first frame the menu shows in, as it opens, has no interval.
    const double interval = g_had_frame ? (double)(at - g_last_frame) / 1e6 : 0.0;
    fprintf(stderr, "[overlay] frame f%ld held %d interval %6.2fms  cpu %6.3fms\n", frame, held ? 1 : 0, interval, cpu);
    g_had_frame = true;
    g_last_frame = at;
}
