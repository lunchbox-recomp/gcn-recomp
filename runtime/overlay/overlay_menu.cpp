// The overlay's menu (overlay.h): its pages, between the header (tabs.h) and the footer
// (footer.h), and the menu's frame, laid out over the dimmed game and moved as it opens or closes.
#include "overlay/overlay.h"
#include "overlay/appear.h"
#include "overlay/footer.h"
#include "overlay/tabs.h"
#include "runtime.h"  // host_ns
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imgui.h"
#include "imgui_impl_sdl3.h"

// overlay.cpp's: whether overlay_init has run, the window the menu is over, and the current
// context's style fitted to the window.
bool overlay_ready();
SDL_Window* overlay_window();
void overlay_style_fit(SDL_Window* window);
// overlay_input.cpp's: the held gamepads read for the menu's frame.
void overlay_read_pads();
// overlay_display.cpp's, overlay_controllers.cpp's and overlay_bindings.cpp's.
void display_page(SDL_Window* window);
void controllers_page();
void bindings_page();

namespace {

std::string g_title;
struct AddedPage {
    std::string title;
    void (*draw)(void* user);
    void* user;
};
std::vector<AddedPage> g_pages;
const std::vector<std::string> kOwnPages = {"Display", "Controllers"};
std::vector<std::string> g_titles = kOwnPages;  // every page's, in order
bool g_focus;             // the menu takes the keyboard's and the gamepads' focus at its next draw
uint64_t g_opened_ns;       // host_ns as it last opened
bool g_said_shown;          // its `shown after` line, since it opened
long g_menu_frames;         // frames that have laid out the menu
ImDrawList* g_panel_list;   // the menu window's draw list at its last frame
int g_panel_vtx;            // and how many of its vertices were the panel's own, before its contents
bool g_on_page;             // its last frame showed a page, not a question or a view

// The page showing, drawn by its own callback.
void page() {
    const int p = tabs_page();
    if (p == 0) display_page(overlay_window());
    else if (p == 1) controllers_page();
    else g_pages[p - 2].draw(g_pages[p - 2].user);
}

// The room between the header, the page and the footer, past an item's own spacing.
void section_gap() {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, (t.section_spacing - 2.0f * t.item_spacing) * s)));
}

// The panel's size: the theme's share of the window's width, at most its widest and inside its
// margin, and as tall as what's on it up to the theme's share of the window's height.
void panel_size() {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;
    const ImVec2 v = ImGui::GetMainViewport()->Size;
    const float w = std::max(1.0f, std::min({t.panel_width * v.x, t.panel_max_width * s, v.x - 2.0f * t.margin * s}));
    const float h = std::max(1.0f, std::min(t.panel_max_height * v.y, v.y - 2.0f * t.margin * s));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, h));
}

void draw_menu(uint64_t now) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (g_focus) ImGui::SetNextWindowFocus();
    g_focus = false;
    panel_size();
    // Begin draws the panel's edge in the Border colour, an item's edge. It takes the theme's
    // fainter panel edge instead. The menu's title is in its header, not a title bar.
    const uint32_t e = theme_now().panel_edge;
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(e >> 24, e >> 16 & 0xFF, e >> 8 & 0xFF, e & 0xFF));
    ImGui::Begin(g_title.c_str(), nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    g_panel_list = ImGui::GetWindowDrawList();
    g_panel_vtx = g_panel_list->VtxBuffer.Size;  // the panel itself, drawn by Begin
    const bool asking = overlay_asking() != OverlayAsk::none;
    g_on_page = !asking && overlay_controllers_asking() < 0 && !overlay_bindings_shown();
    tabs_header(g_title, g_titles, g_on_page, now);
    tabs_no_contents();
    section_gap();
    if (asking) {
        footer_card();
    } else if (overlay_controllers_asking() >= 0) {
        controllers_page();  // the press-A order alone, with nothing to focus
    } else if (overlay_bindings_shown()) {
        bindings_page();
    } else {
        tabs_focus(overlay_open());
        tabs_contents_begin();
        page();
        tabs_contents_end();
    }
    section_gap();
    footer(g_on_page);
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

void menu_init(const char* title) {
    g_title = title;
    tabs_reset();
}

int menu_added_pages() { return (int)g_pages.size(); }

void menu_opened(int page, uint64_t now) {
    g_focus = true;
    if (page >= 0) tabs_show(page);
    g_opened_ns = now;
    g_said_shown = false;
}

// The menu's frame: its input taken while it's open and its windows laid out, ready to draw,
// moved as it opens or closes.
void menu_frame() {
    const uint64_t now = host_ns();
    overlay_style_fit(overlay_window());
    ImGui_ImplSDL3_NewFrame();
    if (overlay_open()) overlay_read_pads();
    ImGui::NewFrame();
    draw_menu(now);
    backdrop(now);
    ImGui::Render();
    tabs_apply(ImGui::GetDrawData(), now);
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
    g_titles.push_back(title);
}

int overlay_page() { return tabs_page(); }

#ifdef GCN_OVERLAY_TEST
void menu_clear_pages() {
    g_pages.clear();
    g_titles = kOwnPages;
}

const std::vector<std::string>& overlay_test_pages() { return g_titles; }

void overlay_test_pick_page(int page) { tabs_pick(page, false); }

bool overlay_test_page_shown() { return g_on_page; }
#endif


long overlay_menu_frames() { return g_menu_frames; }
