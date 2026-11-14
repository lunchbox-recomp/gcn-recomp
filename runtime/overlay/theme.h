// The overlay's look (overlay.h): its colours, its fonts, its sizes, how long its motion takes
// and the colours of controller buttons. The defaults below are the fork's own theme, "Night".
// A game copies overlay_theme_default(), changes what it wants and gives the copy to
// overlay_set_theme before overlay_init; one given after is refused with a line on stderr.
//
// Sizes are pixels at 1080 lines. The menu scales them by its window's height over 1080, and
// builds its style again from the theme whenever that changes.
//
// Some fields are read by nothing yet. Each says so, and which later piece of the look reads
// it: F2 the fonts, F4 the header, tabs and footer, F5 the focus ring, F6 controller buttons'
// glyphs, F7 the notices as toasts and D1 a game's own theme.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A face in a font file: the file, read at run time where the player has it and never shipped,
// and the face's full name in the font's own name table, which finds it in a collection (TTC)
// whatever its order there.
struct OverlayFace {
    std::string path;
    std::string name;
};

// Each kind of text's faces in order of preference, the first that loads taking it (fonts.h).
struct OverlayFonts {
    std::vector<OverlayFace> title, tabs, items, body, footer;
};

// The fork's chains: on macOS the Mac's own Avenir Next, then Helvetica Neue, then Helvetica,
// which has no medium weight: its Bold for the kinds Avenir Next draws in Demi Bold or heavier,
// its regular for the rest. Elsewhere none, so Dear ImGui's own font, until a platform lists its
// own files.
inline OverlayFonts overlay_fonts_default() {
#ifdef __APPLE__
    const std::string avenir = "/System/Library/Fonts/Avenir Next.ttc";
    const std::string neue = "/System/Library/Fonts/HelveticaNeue.ttc";
    const std::string helvetica = "/System/Library/Fonts/Helvetica.ttc";
    return {{{avenir, "Avenir Next Heavy"}, {neue, "Helvetica Neue Bold"}, {helvetica, "Helvetica Bold"}},
            {{avenir, "Avenir Next Demi Bold"}, {neue, "Helvetica Neue Medium"}, {helvetica, "Helvetica Bold"}},
            {{avenir, "Avenir Next Medium"}, {neue, "Helvetica Neue Medium"}, {helvetica, "Helvetica"}},
            {{avenir, "Avenir Next Medium"}, {neue, "Helvetica Neue"}, {helvetica, "Helvetica"}},
            {{avenir, "Avenir Next Demi Bold"}, {neue, "Helvetica Neue Medium"}, {helvetica, "Helvetica Bold"}}};
#else
    return {};
#endif
}

struct OverlayTheme {
    // Colours, each 0xRRGGBBAA. Text and dimmed text stay at least 4.5:1 against every fill they
    // sit on, and the focus ring and an item's edge 3:1 against the panel, over any game
    // (tools/test_overlay_theme.cpp). The edge is all an unchecked box or a slider's track shows.
    // No text sits on a slider's grab, the accent, which is light enough to stand out from the
    // track at 3:1 and so too light for text: the menu's sliders have their values beside them.
    uint32_t backdrop = 0x0B0E148C;      // over the game while the menu is open, 55%
    uint32_t panel = 0x141925EB;         // the menu's panel, 92%
    uint32_t item = 0x1E2533FF;          // a button, box or tab at rest
    uint32_t focused = 0x2A3346FF;       // the same with the focus on it
    uint32_t pressed = 0x344059FF;       // the same held down
    uint32_t border = 0xFFFFFF66;        // the edge of each item, white at 40%
    // The panel's own edge and the rule under its title, white at 15%: fainter than an item's
    // edge, so the rule, as wide as the panel, isn't its strongest line.
    uint32_t panel_edge = 0xFFFFFF26;
    uint32_t divider = 0xFFFFFF0F;       // a line between sections, white at 6%
    uint32_t text = 0xF2F4F8FF;
    uint32_t dim = 0xA9B1C2FF;           // text that says less, as a note does
    // The text of what can't be picked. Not read yet, and no piece reads it so far: Dear ImGui
    // fades what can't be picked by its style's DisabledAlpha instead.
    uint32_t disabled = 0x5C6578FF;
    uint32_t accent = 0x3DD6F5FF;        // what's picked, a mark and the focus ring
    uint32_t accent_light = 0xA6EEFBFF;  // the ring's pulse (F5) and a grab held
    uint32_t accent_dark = 0x177A90FF;   // the selected tab, under its text
    // A second accent for a theme that wants one (D1), here the accent, and success and warning,
    // none read yet; danger under the confirm of a question's card, its text the panel's colour.
    uint32_t secondary = 0x3DD6F5FF;
    uint32_t success = 0x4ADE80FF;
    uint32_t warning = 0xFACC15FF;
    uint32_t danger = 0xF87171FF;

    // The faces each kind of text tries, at the text sizes below. A kind none of whose faces
    // loads, or that lists none, takes Dear ImGui's own embedded font.
    OverlayFonts fonts = overlay_fonts_default();

    // The panel, centred in the window: panel_width of the window's width, and as tall as its
    // page up to panel_max_height of it, past which the page scrolls.
    float panel_width = 0.62f;        // of the window's width, at most panel_max_width
    float panel_max_width = 1100.0f;
    float panel_max_height = 0.9f;    // of the window's height
    float margin = 64.0f;             // the least room between the panel and the window's edge
    float padding = 32.0f;            // inside the panel
    float panel_radius = 20.0f;
    float border_size = 1.0f;         // the panel's edge and each item's, at least a pixel
    // What's on it.
    float item_height = 56.0f;        // a button, box or tab
    float item_min_height = 48.0f;    // nothing to press is shorter, and a page's pill this tall
    float item_radius = 12.0f;
    float item_spacing = 8.0f;
    float section_spacing = 24.0f;    // between the header, the page and the footer
    // The focus ring (ring.h) around the item with the focus: focus_width wide, past WCAG 2.2's
    // 2 px for a screen at arm's length since the menu is read from across a room, and focus_gap
    // outside the item, so it never touches the item's own edge.
    float focus_width = 3.0f;
    float focus_gap = 2.0f;
    // Text sizes, the fonts' roles (F2): the menu's title, the pills, the buttons of a question's
    // card, the body, and the footer's hints and buttons. A page's own items take the body's.
    float title_size = 36.0f;
    float tab_size = 26.0f;
    float item_text_size = 24.0f;
    float body_size = 22.0f;
    float footer_size = 18.0f;

    // Motion, in milliseconds of host time.
    float backdrop_ms = 200.0f;       // the backdrop dims in
    float open_ms = 180.0f;           // the panel fades in and grows from open_scale
    float open_scale = 0.96f;
    float open_overshoot = 1.2f;      // how far past full size the panel grows before it settles
    float contents_delay_ms = 40.0f;  // the page's contents follow the panel
    float close_ms = 120.0f;          // the panel fades out and shrinks to close_scale
    float close_scale = 0.98f;
    float page_ms = 160.0f;           // a page change (tabs.h), its contents sliding page_slide pixels
    float page_slide = 32.0f;
    float focus_ms = 45.0f;           // the focus ring's time constant as it follows a move (ring.h)
    // The rest aren't read yet: the focus ring's pulse and press, and the toasts (F7).
    float pulse_ms = 1200.0f;         // one breath of the focus ring
    float press_scale = 0.98f;        // an item held down, springing back over press_ms
    float press_ms = 120.0f;
    float toast_in_ms = 200.0f;       // a notice rising toast_rise pixels as it fades in
    float toast_out_ms = 180.0f;      // and fading out at the end of its time
    float toast_rise = 16.0f;

    // Controller buttons' colours, each 0xRRGGBBAA, by the face the game takes each as: A, B, X
    // and Y (gamepads_faces in gamepads.h). A Nintendo controller's faces are drawn in the text's
    // colour. Not read yet: the glyphs (F6) draw with them.
    struct Faces {
        uint32_t a, b, x, y;
    };
    Faces xbox = {0x3CB043FF, 0xE03A3EFF, 0x2F7BE5FF, 0xF2B705FF};
    Faces playstation = {0x7AA8FFFF, 0xFF6B6BFF, 0xE58CFFFF, 0x3DDC97FF};  // cross, circle, square, triangle
    Faces gamecube = {0x1FB57AFF, 0xE0463AFF, 0xBFC5CFFF, 0xBFC5CFFF};
    uint32_t gamecube_z = 0x6C5CE7FF;
};

// The fork's theme, Night.
inline OverlayTheme overlay_theme_default() { return OverlayTheme{}; }

#ifdef GCN_OVERLAY
// Before overlay_init, the theme the overlay draws with. After it, one line on stderr and nothing
// changes.
void overlay_set_theme(const OverlayTheme& theme);
#else
inline void overlay_set_theme(const OverlayTheme&) {}
#endif

// The overlay's own, theme.cpp's: the theme, and the current Dear ImGui context's style built
// from it at a scale, the scale for a window's height in Dear ImGui's units, and the style built
// again where that has changed.
const OverlayTheme& theme_now();
void theme_set(const OverlayTheme& theme);
void theme_style(const OverlayTheme& theme, float scale);
float theme_scale(int height);
void theme_fit(int height);
// One of the theme's 0xRRGGBBAA colours as Dear ImGui packs one (IM_COL32), its ImU32.
uint32_t theme_colour(uint32_t rgba);
