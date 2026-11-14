// The overlay's menu (overlay.h): its pages, between the header (tabs.h) and the footer
// (footer.h), and the menu's frame, laid out over the dimmed game and moved as it opens or closes.
#include "overlay/overlay.h"
#include "overlay/appear.h"
#include "overlay/footer.h"
#include "overlay/ring.h"
#include "overlay/state.h"
#include "overlay/tabs.h"
#include "runtime.h"  // host_ns
#include "ui_sound.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"  // where the focus moved to, and what was pressed

// overlay.cpp's: the current context's style fitted to the window.
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
// A game adds its pages from a static initializer in its SOURCES, which may run before this
// file's. So the added pages are constant-initialized, and every page's title, in order, is
// function-local: built at namespace scope, it would be built again over the game's titles
// (vr_config.cpp's hooks()).
std::vector<AddedPage> g_pages;
constexpr const char* kOwnPages[] = {"Display", "Controllers"};
std::vector<std::string>& titles() {
    static std::vector<std::string> t(std::begin(kOwnPages), std::end(kOwnPages));
    return t;
}
bool g_focus;            // the menu takes the keyboard's and the gamepads' focus at its next draw
uint64_t g_opened_ns;       // host_ns as it last opened
bool g_said_shown;          // its `shown after` line, since it opened
long g_menu_frames;         // frames that have laid out the menu
ImDrawList* g_panel_list;   // the menu window's draw list at its last frame
int g_panel_vtx;            // and how many of its vertices were the panel's own, before its contents
bool g_on_page;             // its last frame showed a page, not a question or a view
// The panel's height at its tallest for a page since the menu opened, its least from then on, in
// pixels at the theme's scale then (panel_place). A view or a question is left out, so backing
// out of one puts the page back in a panel its pages' size.
float g_tallest, g_tallest_scale;
float g_most;     // the panel's greatest height at this frame, in pixels (panel_place)
bool g_rescaled;  // and whether the theme's scale changed at this frame, with the window's height
// The Buttons view's height at the frame laid out last, at 1080 lines as the theme's sizes are, so
// it holds across a change of the window's height; -1 with no view then.
float g_view = -1.0f;
ImGuiID g_region;  // the window of the region the view scrolls in, once one has been laid out (view)

// The page showing, drawn by its own callback.
void page() {
    const int p = tabs_page();
    if (p == 0) display_page(overlay_window());
    else if (p == 1) controllers_page();
    else g_pages[p - 2].draw(g_pages[p - 2].user);
}

// The room between the header, the page and the footer, past an item's own spacing.
float gap() {
    const OverlayTheme& t = theme_now();
    return std::max(0.0f, (t.section_spacing - 2.0f * t.item_spacing) * ImGui::GetStyle().FontScaleMain);
}
void section_gap() { ImGui::Dummy(ImVec2(0.0f, gap())); }

// The panel's place and size: centred in the window, the theme's share of its width, at most its
// widest and inside its margin, and as tall as what's on it up to the theme's share of its
// height, but never shorter than a page has had it since the menu opened (g_tallest). So its
// top is half its tallest yet above the window's middle: a page no taller than one shown since
// it opened leaves the header where it was, and a taller one grows the panel by half its extra
// each way.
// Centring it by Dear ImGui's pivot places it by its height at this frame, a new tallest too.
void panel_place() {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;
    g_rescaled = s != g_tallest_scale;
    if (g_rescaled) {  // a new window height lays it all out again (menu_opened)
        g_tallest = 0.0f;
        g_on_page = false;
    }
    g_tallest_scale = s;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 v = vp->Size;
    const float w = std::max(1.0f, std::min({t.panel_width * v.x, t.panel_max_width * s, v.x - 2.0f * t.margin * s}));
    const float h = std::max(1.0f, std::min(t.panel_max_height * v.y, v.y - 2.0f * t.margin * s));
    g_most = h;
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, std::min(g_tallest, h)), ImVec2(w, h));
}

// The Buttons view in place of a page, in a region of its own that scrolls: as tall as the view
// was at the last frame, or at its first the most it can be, up to the room the panel at its
// tallest leaves under the header once the gap and the footer have theirs, so the footer stays on
// the panel's bottom, past the region. Those are taken at this frame's scale, so a change of the
// window's height lays the region out again at once. That first frame sizes the panel at the
// next as a page's does; a region Dear ImGui sized to its contents would show nothing at its
// first. Dear ImGui scrolls the region to keep the item with the focus in it, however the focus
// got there, as it scrolls the panel itself for a page taller than the panel, and the view scrolls
// it back to that item as a change of the window's height moves the rows. The region's items
// take the focus as the panel's own (NavFlattened), so a click on the panel past the region
// leaves the focus on its row, not on the region as a whole. The region is the panel's width,
// with its padding across and its items' width, so the view is laid out across as it would be on
// the panel, its scroll bar where the panel's would be, and widens nothing the panel could
// scroll across to. The footer's rule then ends where the rows' rules do, short of that bar.
void view(float last) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    const float below = 2.0f * ImGui::GetStyle().ItemSpacing.y + gap() + footer_height();
    const float room = std::floor(g_most - 2.0f * pad.y - (at.y - w->DC.CursorStartPos.y) - below);
    const float s = ImGui::GetStyle().FontScaleMain;
    const float tall = last < 0.0f ? room : std::min(room, std::ceil(last * s - 1e-3f));  // less a float's error
    ImGui::SetCursorScreenPos(ImVec2(w->Pos.x, at.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad.x, 0.0f));
    ImGui::BeginChild("view", ImVec2(w->Size.x, std::max(tall, 1.0f)),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    ImGui::PopStyleVar();
    ImGui::PushItemWidth(w->DC.ItemWidth);  // the panel's, which a region its width wouldn't take
    bindings_page();
    ImGui::PopItemWidth();
    ImGuiWindow* region = ImGui::GetCurrentWindow();
    g_region = region->ID;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g_rescaled && g.NavWindow == region && g.NavIdIsAlive)
        ImGui::ScrollToRectEx(region, ImGui::WindowRectRelToAbs(region, region->NavRectRel[ImGuiNavLayer_Main]),
                              ImGuiScrollFlags_KeepVisibleEdgeY | ImGuiScrollFlags_NoScrollParent);
    g_view = (region->DC.CursorMaxPos.y - region->DC.CursorStartPos.y) / s;
    const float right = region->WorkRect.Max.x;
    ImGui::EndChild();
    w->DC.CursorMaxPos.x = std::min(w->DC.CursorMaxPos.x, w->WorkRect.Max.x);
    w->WorkRect.Max.x = right;
}

void draw_menu(uint64_t now) {
    // Opening shows where the focus is, so A presses it and a move goes from it, after the mouse
    // too, which hides it, and at the session's first open, where nothing has shown it yet.
    if (g_focus) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNavCursorVisible(true);
    }
    g_focus = false;
    panel_place();
    // Begin draws the panel's edge in the Border colour, an item's edge. It takes the theme's
    // fainter panel edge instead. The menu's title is in its header, not a title bar.
    ImGui::PushStyleColor(ImGuiCol_Border, theme_colour(theme_now().panel_edge));
    ImGui::Begin(g_title.c_str(), nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    // Dear ImGui sizes the panel by what the frame before laid out in it, so its height is a
    // page's only if that frame showed one.
    if (g_on_page) g_tallest = std::max(g_tallest, ImGui::GetWindowHeight());
    // The panel keeps the item with the focus in the Buttons view's region (view) as its own, as
    // it would with the view on it. Once the region goes, Dear ImGui hands the focus back to the
    // panel's, which would otherwise be the button that put the view up, and a close's fade would
    // move it there. Only the region's: a popup a page opens, a combo's list, is the panel's too,
    // and its item is gone once it closes.
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.NavWindow && g.NavWindow->ID == g_region) ImGui::GetCurrentWindow()->NavLastIds[ImGuiNavLayer_Main] = g.NavId;
    g_panel_list = ImGui::GetWindowDrawList();
    g_panel_vtx = g_panel_list->VtxBuffer.Size;  // the panel itself, drawn by Begin
    const bool asking = overlay_asking() != OverlayAsk::none;
    g_on_page = !asking && overlay_controllers_asking() < 0 && !overlay_bindings_shown();
    tabs_header(g_title, titles(), g_on_page, now);
    tabs_no_contents();
    section_gap();
    const float view_was = g_view;
    g_view = -1.0f;
    if (asking) {
        footer_card();
    } else if (overlay_controllers_asking() >= 0) {
        controllers_page();  // the press-A order alone, with nothing to focus
    } else if (overlay_bindings_shown()) {
        view(view_was);
    } else {
        tabs_focus(overlay_open());
        tabs_contents_begin();
        page();
        tabs_contents_end();
    }
    section_gap();
    footer(g_on_page);
    ring_draw(now);
    ImGui::End();
}

// The game dimmed behind the menu, in the background draw list, which the panel's motion leaves
// alone (appear_apply).
void backdrop(uint64_t now) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const uint32_t c = theme_now().backdrop;
    const uint32_t alpha = (uint32_t)((float)(c & 0xFF) * appear_backdrop(now) + 0.5f);
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  theme_colour((c & ~0xFFu) | alpha));
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
    // The panel starts again at the page it opens on. Its first frame is sized by the last one
    // laid out before, which may be another page's, at another window height, so it reads none.
    g_tallest = 0.0f;
    g_on_page = false;
    ring_snap();  // and the focus ring on its item
}

// The menu's frame: its input taken while it's open and its windows laid out, ready to draw,
// moved as it opens or closes.
void menu_frame() {
    const uint64_t now = host_ns();
    overlay_style_fit(overlay_window());
    ImGui_ImplSDL3_NewFrame();
    if (overlay_open()) overlay_read_pads();
    // The focus Dear ImGui puts on a page's first item, as the menu opens or a page turns, is no
    // move of the player's, so it doesn't tick.
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool placed = g.NavInitRequest || g.NavInitResult.ID != 0;
    ImGui::NewFrame();
    const bool moved = g.NavJustMovedToId != 0 && !placed;
    draw_menu(now);
    if (overlay_open() && g.ActiveIdIsJustActivated && g.ActiveId != 0) ui_sound_play(UiSound::confirm);
    else if (overlay_open() && moved) ui_sound_play(UiSound::move);
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
    titles().push_back(title);
}

#ifdef GCN_OVERLAY_TEST
int overlay_page() { return tabs_page(); }

void menu_clear_pages() {
    g_pages.clear();
    titles().assign(std::begin(kOwnPages), std::end(kOwnPages));
}

const std::vector<std::string>& overlay_test_pages() { return titles(); }

void overlay_test_pick_page(int page) { tabs_pick(page, false); }

bool overlay_test_page_shown() { return g_on_page; }
#endif


long overlay_menu_frames() { return g_menu_frames; }
