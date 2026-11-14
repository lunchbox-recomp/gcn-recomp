// The menu's footer, Restart game and Quit game, and what they ask (footer.h).
#include "overlay/footer.h"
#include "overlay/fonts.h"
#include "overlay/overlay.h"
#include "overlay/state.h"
#include "overlay/theme.h"
#include "frontend.h"
#include "input_log.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include "imgui.h"
#include "imgui_internal.h"  // ImGui::FocusItem

namespace {

OverlayAsk g_ask;     // what the menu is asking before it does it
OverlayAsk g_asked;   // the button that asked, which takes the focus back at the next draw
bool g_focus_cancel;  // Cancel takes the focus at the next draw, as the menu starts asking

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

// A key's name and what it does there. The names are plain text until F6's glyphs draw each key
// as the device last pressed labels it. Drafts for the user.
struct Hint {
    const char* key;
    const char* does;
};
constexpr Hint kOnPage[] = {{"A", "Select"}, {"B", "Back"}, {"L R", "Page"}, {"F1", "Close"}};
constexpr Hint kInView[] = {{"A", "Select"}, {"B", "Back"}, {"F1", "Close"}};
// While the Buttons view listens for a press to bind, every button on its gamepad is the
// binding's, A and B and the shoulders too, so only its way out: Back on any gamepad, Guide too
// (overlay_bindings.cpp), or Escape.
constexpr Hint kListening[] = {{"Back", "Cancel"}, {"Esc", "Cancel"}};

#ifdef GCN_OVERLAY_TEST
std::string g_hints;  // the hints last drawn, each key and what it does, for the tests
#endif

// The hints from `at`, centred in a row `height` tall, and how wide they are.
template <size_t N>
float hints(const Hint (&list)[N], ImVec2 at, float height) {
    const OverlayTheme& t = theme_now();
    const float gap = t.item_spacing * ImGui::GetStyle().FontScaleMain;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = at.y + (height - ImGui::GetFontSize()) / 2.0f;
    float x = at.x;
#ifdef GCN_OVERLAY_TEST
    g_hints.clear();
    for (const Hint& h : list) g_hints += std::string(g_hints.empty() ? "" : ", ") + h.key + " " + h.does;
#endif
    for (const Hint& h : list) {
        if (x > at.x) x += 3.0f * gap;
        dl->AddText(ImVec2(x, y), theme_colour(t.text), h.key);
        x += ImGui::CalcTextSize(h.key).x + gap;
        dl->AddText(ImVec2(x, y), theme_colour(t.dim), h.does);
        x += ImGui::CalcTextSize(h.does).x;
    }
    return x - at.x;
}

}  // namespace

void footer(bool on_page) {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;
    const float height = t.item_min_height * s;
    ImGui::Separator();
    ImGui::PushFont(fonts_role(FontRole::footer), t.footer_size);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float room = ImGui::GetContentRegionAvail().x;
    const float wide = overlay_bindings_listening() ? hints(kListening, at, height)
                       : on_page                    ? hints(kOnPage, at, height)
                                                    : hints(kInView, at, height);
    if (!on_page) {
        ImGui::Dummy(ImVec2(wide, height));
        ImGui::PopFont();
        return;
    }
    // Small, in the footer's text, but no shorter than anything else to press.
    const ImVec2 frame(t.padding / 2.0f * s, std::max(0.0f, (t.item_min_height - t.footer_size) / 2.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, frame);
    const bool restart = !input_replay_loaded();
    float buttons = ImGui::CalcTextSize("Quit game").x + 2.0f * frame.x;
    if (restart) buttons += ImGui::CalcTextSize("Restart game").x + 2.0f * frame.x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorScreenPos(ImVec2(std::max(at.x + wide, at.x + room - buttons), at.y));
    // A replay has no Restart game, even once its log runs out: the log can't hold one, and a
    // relaunch would replay the log from its start (frontend_restart).
    if (restart) {
        if (ImGui::Button("Restart game")) ask(OverlayAsk::restart);
        if (overlay_open() && g_asked == OverlayAsk::restart) ImGui::FocusItem();
        ImGui::SameLine();
    }
    if (ImGui::Button("Quit game")) ask(OverlayAsk::quit);
    if (overlay_open() && g_asked == OverlayAsk::quit) ImGui::FocusItem();
    g_asked = OverlayAsk::none;
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

void footer_card() {
    const OverlayTheme& t = theme_now();
    const float s = ImGui::GetStyle().FontScaleMain;
    const bool restart = g_ask == OverlayAsk::restart;
    const char* question = restart ? "Restart the game? Anything it hasn't saved is lost."
                                   : "Quit the game? Anything it hasn't saved is lost.";
    const char* confirm = restart ? "Restart" : "Quit";
    const float pad = t.padding * s;
    const ImVec2 frame(t.padding / 2.0f * s, std::max(0.0f, (t.item_height - t.item_text_size) / 2.0f * s));
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::PushFont(fonts_role(FontRole::items), t.item_text_size);
    const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize(confirm).x + 4.0f * frame.x + spacing;
    ImGui::PopFont();
    const float said = ImGui::CalcTextSize(question).x;
    const float inner = std::max(said, buttons);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float left = at.x + std::max(0.0f, (ImGui::GetContentRegionAvail().x - inner - 2.0f * pad) / 2.0f);

    // The card is drawn behind what's on it once that's laid out and its height is known.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(left + pad + (inner - said) / 2.0f, at.y + pad));
    ImGui::TextUnformatted(question);
    ImGui::SetCursorScreenPos(ImVec2(left + pad + (inner - buttons) / 2.0f, ImGui::GetCursorScreenPos().y));
    ImGui::PushFont(fonts_role(FontRole::items), t.item_text_size);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, frame);
    if (ImGui::Button("Cancel")) footer_back();
    if (overlay_open() && g_focus_cancel) ImGui::FocusItem();
    g_focus_cancel = false;
    ImGui::SameLine();
    // The confirm on the danger colour, its text the panel's colour, whole.
    for (ImGuiCol c : {ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive})
        ImGui::PushStyleColor(c, theme_colour(t.danger));
    ImGui::PushStyleColor(ImGuiCol_Text, theme_colour(t.panel | 0xFF));
    if (ImGui::Button(confirm)) {
        if (restart) frontend_restart();
        else quit();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    const ImVec2 lo(left, at.y), hi(left + inner + 2.0f * pad, ImGui::GetItemRectMax().y + pad);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(lo, hi, theme_colour(t.item), t.item_radius * s);
    dl->AddRect(lo, hi, theme_colour(t.panel_edge), t.item_radius * s, 0, ImGui::GetStyle().WindowBorderSize);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(left, hi.y));
    ImGui::Dummy(ImVec2(hi.x - lo.x, 0.0f));
}

void footer_back() {
    g_asked = g_ask;
    g_ask = OverlayAsk::none;
}

void footer_end_ask() { g_ask = g_asked = OverlayAsk::none; }

OverlayAsk overlay_asking() { return g_ask; }

#ifdef GCN_OVERLAY_TEST
const std::string& footer_test_hints() { return g_hints; }
#endif
