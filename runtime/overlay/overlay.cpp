// The overlay's set-up, opening and closing, and its frames; see overlay.h. What opens and
// closes it and the input it holds back are overlay_input.cpp, the menu itself overlay_menu.cpp
// with its header tabs.cpp and its footer footer.cpp, the Display page overlay_display.cpp, the
// Controllers page overlay_controllers.cpp and the notices notices.cpp.
#include "overlay/overlay.h"
#include "overlay/appear.h"
#include "overlay/fonts.h"
#include "overlay/footer.h"
#include "overlay/review.h"
#include "overlay/state.h"
#include "overlay/tabs.h"
#include "gx/render_gl.h"
#include "pause.h"
#include "runtime.h"  // host_ns
#include <SDL3/SDL.h>
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

bool g_ready, g_open;
bool g_view_left;  // it closed from a view in place of a page, drawn as it fades (overlay_set_open)
SDL_Window* g_window;  // null in tests, which draw nothing
bool g_gl;             // the window has a GL context to draw the menu with, which tests' don't
bool g_dumped;            // since it opened, under --dump-output
long long g_at_frame = -1;  // GCN_OVERLAY_AT's, -1 for none or once it's opened the overlay
long g_at_left;             // draws over the held frame left before GCN_OVERLAY_AT's closes it
int g_page = -1;            // GCN_OVERLAY_PAGE's, 0 for Display, -1 for none
bool g_plain;               // GCN_OVERLAY_PLAIN: dumps over plain grey, not the held frame
ReviewWatch g_watch;        // the menu's frame settling for its dump, from each open

// What showed in place of a page ends with nothing done: Restart? or Quit?, the press-A order,
// and the Buttons view with any press-to-bind.
void end_view() {
    g_view_left = false;
    footer_end_ask();
    controllers_stop();
    bindings_stop();
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
    notices_motion(motion);
    if (const char* e = getenv_nonempty("GCN_OVERLAY_AT")) review_parse_at(e, g_at_frame, g_at_left);
    const char* page = getenv_nonempty("GCN_OVERLAY_PAGE");
    g_page = page ? review_parse_page(page, 1 + menu_added_pages()) : -1;
    g_plain = getenv_nonempty("GCN_OVERLAY_PLAIN") != nullptr;
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

bool overlay_menu_due() { return menu_showing() || at_frame_due(); }

bool overlay_animating() {
    const uint64_t now = host_ns();
    return g_window && (appear_moving(now) || (g_open && tabs_moving(now)) || notices_moving(now));
}

void overlay_close() { overlay_set_open(false); }

void overlay_menu_frame() {
    if (!g_window) return;
    fonts_new_frame();
    if (menu_lays_out()) menu_frame();
    notices_frame();
}

void overlay_draw() {
    if (!g_window || !g_gl) return;
    fonts_new_frame();  // once a frame, before the menu's and the notices' (fonts.h)
    // Opening ends a view left from the last close itself (overlay_set_open).
    if (at_frame_due()) {
        g_at_frame = -1;
        overlay_set_open(true);
    }
    if (menu_lays_out()) {
        ImGui_ImplOpenGL3_NewFrame();
        menu_frame();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    notices_frame();
    if (!g_open || !paused()) return;
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
}
