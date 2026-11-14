// The overlay's menu (overlay.h): its pages, Restart game and Quit game and what they ask, and
// the menu's frame, laid out over the dimmed game and moved as it opens or closes.
#include "overlay/overlay.h"
#include "overlay/appear.h"
#include "frontend.h"
#include "input_log.h"
#include "runtime.h"  // host_ns
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"  // ImGui::FocusItem

// overlay.cpp's: whether overlay_init has run, the window the menu is over, the menu opened or
// closed, and the current context's style fitted to the window.
bool overlay_ready();
SDL_Window* overlay_window();
void overlay_set_open(bool on);
void overlay_style_fit(SDL_Window* window);
// overlay_input.cpp's: the held gamepads read for the menu's frame.
void overlay_read_pads();
// overlay_display.cpp's, overlay_controllers.cpp's and overlay_bindings.cpp's.
void display_page(SDL_Window* window);
void controllers_page();
void bindings_page();
// Below, for overlay_input.cpp too: back from a question to the button that asked.
void menu_back();

namespace {

std::string g_title;
struct AddedPage {
    std::string title;
    void (*draw)(void* user);
    void* user;
};
std::vector<AddedPage> g_pages;
OverlayAsk g_ask;         // what the menu is asking before it does it
OverlayAsk g_asked;       // the button that asked, which takes the focus back at the next draw
bool g_focus_cancel;      // Cancel takes the focus at the next draw, as the menu starts asking
bool g_focus;             // the menu takes the keyboard's and the gamepads' focus at its next draw
int g_select = -1;          // the page the next draw selects, as the menu opens on GCN_OVERLAY_PAGE's
uint64_t g_opened_ns;       // host_ns as it last opened
bool g_said_shown;          // its `shown after` line, since it opened
long g_menu_frames;         // frames that have laid out the menu
ImDrawList* g_panel_list;   // the menu window's draw list at its last frame
int g_panel_vtx;            // and how many of its vertices were the panel's own, before its contents

// A page's tab, selected by the draw after the menu opens where GCN_OVERLAY_PAGE gave it.
bool page_tab(const char* title, int index) {
    return ImGui::BeginTabItem(title, nullptr, g_select == index ? ImGuiTabItemFlags_SetSelected : 0);
}

// The menu asks before it restarts or quits, with Cancel focused, and goes back to the button
// that asked. FocusItem moves the focus without pressing what it lands on, which
// SetKeyboardFocusHere does.
void ask(OverlayAsk what) {
    g_ask = what;
    g_focus_cancel = true;
}

// Closes the overlay and asks SDL to quit, as the window's close button does, so the frontend's
// loop stops and the run ends the way it does for an Escape.
void quit() {
    overlay_set_open(false);
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

// Below the pages, so on every one. A replay has no Restart game, even once its log runs out:
// the log can't hold one, and a relaunch would replay the log from its start (frontend_restart).
void actions() {
    ImGui::Separator();
    if (!input_replay_loaded()) {
        if (ImGui::Button("Restart game")) ask(OverlayAsk::restart);
        if (overlay_open() && g_asked == OverlayAsk::restart) ImGui::FocusItem();
        ImGui::SameLine();
    }
    if (ImGui::Button("Quit game")) ask(OverlayAsk::quit);
    if (overlay_open() && g_asked == OverlayAsk::quit) ImGui::FocusItem();
    g_asked = OverlayAsk::none;
}

void asking() {
    const bool restart = g_ask == OverlayAsk::restart;
    ImGui::TextUnformatted(restart ? "Restart the game? Anything it hasn't saved is lost."
                                   : "Quit the game? Anything it hasn't saved is lost.");
    if (ImGui::Button("Cancel")) menu_back();
    if (overlay_open() && g_focus_cancel) ImGui::FocusItem();
    g_focus_cancel = false;
    ImGui::SameLine();
    if (ImGui::Button(restart ? "Restart" : "Quit")) {
        if (restart) frontend_restart();
        else quit();
    }
}

void draw_menu() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (g_focus) ImGui::SetNextWindowFocus();
    g_focus = false;
    // Begin draws the panel's edge, and the rule under its title that a frame border turns on, in
    // the Border colour, an item's edge. They take the theme's fainter panel edge instead.
    const uint32_t e = theme_now().panel_edge;
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(e >> 24, e >> 16 & 0xFF, e >> 8 & 0xFF, e & 0xFF));
    ImGui::Begin(g_title.c_str(), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    g_panel_list = ImGui::GetWindowDrawList();
    g_panel_vtx = g_panel_list->VtxBuffer.Size;  // the panel itself, drawn by Begin
    if (g_ask != OverlayAsk::none) {
        asking();
    } else if (overlay_controllers_asking() >= 0) {
        controllers_page();  // the press-A order alone, with nothing to focus
    } else if (overlay_bindings_shown()) {
        bindings_page();
    } else {
        if (ImGui::BeginTabBar("pages")) {
            if (page_tab("Display", 0)) {
                display_page(overlay_window());
                ImGui::EndTabItem();
            }
            if (page_tab("Controllers", 1)) {
                controllers_page();
                ImGui::EndTabItem();
            }
            for (size_t i = 0; i < g_pages.size(); i++) {
                if (page_tab(g_pages[i].title.c_str(), 2 + (int)i)) {
                    g_pages[i].draw(g_pages[i].user);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
            g_select = -1;
        }
        actions();
    }
    ImGui::End();
}

// The game dimmed behind the menu, in the background draw list, which the panel's motion leaves
// alone (appear_apply).
void backdrop(uint64_t now) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const uint32_t c = theme_now().backdrop;
    const int alpha = (int)((float)(c & 0xFF) * appear_backdrop(now) + 0.5f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(c >> 24, c >> 16 & 0xFF, c >> 8 & 0xFF, alpha));
}

// Once the menu has opened, its panel drawn whole for the first time.
void say_shown(uint64_t now) {
    if (g_said_shown || !overlay_open() || !appear_shown(now) || !ImGui::GetDrawData()->CmdLists.contains(g_panel_list))
        return;
    g_said_shown = true;
    fprintf(stderr, "[overlay] shown after %llu ms\n", (unsigned long long)((now - g_opened_ns) / 1000000));
}

}  // namespace

void menu_title(const char* title) { g_title = title; }

int menu_added_pages() { return (int)g_pages.size(); }

void menu_opened(int page, uint64_t now) {
    g_focus = true;
    g_select = page;
    g_opened_ns = now;
    g_said_shown = false;
}

void menu_back() {
    g_asked = g_ask;
    g_ask = OverlayAsk::none;
}

void menu_end_ask() { g_ask = g_asked = OverlayAsk::none; }

// The menu's frame: its input taken while it's open and its windows laid out, ready to draw,
// moved as it opens or closes.
void menu_frame() {
    const uint64_t now = host_ns();
    overlay_style_fit(overlay_window());
    ImGui_ImplSDL3_NewFrame();
    if (overlay_open()) overlay_read_pads();
    ImGui::NewFrame();
    draw_menu();
    backdrop(now);
    ImGui::Render();
    const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    appear_apply(ImGui::GetDrawData(), ImGui::GetBackgroundDrawList(), g_panel_list, g_panel_vtx, c.x, c.y,
                 appear_scale(now), appear_panel_alpha(now), appear_contents_alpha(now));
    g_menu_frames++;
    say_shown(now);
}

void overlay_add_page(const char* title, void (*draw)(void* user), void* user) {
    if (overlay_ready()) { fprintf(stderr, "[overlay] page registration is closed after init\n"); return; }
    if (!title || !*title) { fprintf(stderr, "[overlay] page title is empty\n"); return; }
    if (!strcmp(title, "Display") || !strcmp(title, "Controllers") ||
        std::any_of(g_pages.begin(), g_pages.end(), [&](const AddedPage& page) { return page.title == title; })) {
        fprintf(stderr, "[overlay] page title %s is already in use\n", title);
        return;
    }
    if (!draw) { fprintf(stderr, "[overlay] page %s has no callback\n", title); return; }
    g_pages.push_back({title, draw, user});
}

#ifdef GCN_OVERLAY_TEST
void menu_clear_pages() { g_pages.clear(); }
#endif

OverlayAsk overlay_asking() { return g_ask; }

long overlay_menu_frames() { return g_menu_frames; }
