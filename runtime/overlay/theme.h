// The overlay's look (overlay.h): its colours, its fonts, its sizes, how long its motion takes
// and the colours of controller buttons. The defaults below are the fork's own theme, "Night".
// A game copies overlay_theme_default(), changes what it wants and gives the copy to
// overlay_set_theme before overlay_init; one given after is refused with a line on stderr.
//
// Sizes are pixels at 1080 lines. The menu scales them by its window's height over 1080, and
// builds its style again from the theme whenever that changes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct OverlayTheme {
    // Colours, each 0xRRGGBBAA. Text and dimmed text stay at least 4.5:1 against every fill they
    // sit on, and the focus ring 3:1 against the panel, over any game (tools/test_overlay_theme.cpp).
    uint32_t backdrop = 0x0B0E148C;      // over the game while the menu is open, 55%
    uint32_t panel = 0x141925EB;         // the menu's panel, 92%
    uint32_t item = 0x1E2533FF;          // a button, box or tab at rest
    uint32_t focused = 0x2A3346FF;       // the same with the focus on it
    uint32_t pressed = 0x344059FF;       // the same held down
    uint32_t border = 0xFFFFFF1A;        // the panel's edge, white at 10%
    uint32_t divider = 0xFFFFFF0F;       // a line between sections, white at 6%
    uint32_t text = 0xF2F4F8FF;
    uint32_t dim = 0xA9B1C2FF;           // text that says less, as a note does
    uint32_t disabled = 0x5C6578FF;      // the text of what can't be picked
    uint32_t accent = 0x3DD6F5FF;        // what's picked, a mark and the focus ring
    uint32_t accent_light = 0xA6EEFBFF;  // the ring's pulse and a grab held
    uint32_t accent_dark = 0x177A90FF;   // the selected tab, under its text
    uint32_t secondary = 0x3DD6F5FF;     // a second accent for a theme that wants one; here the accent
    uint32_t success = 0x4ADE80FF;
    uint32_t warning = 0xFACC15FF;
    uint32_t danger = 0xF87171FF;

    // Font files in order of preference, the first that loads taking the text. Empty for Dear
    // ImGui's own embedded font, the only one used so far.
    std::vector<std::string> fonts;

    // The panel, centred in the window.
    float panel_width = 0.62f;        // of the window's width, at most panel_max_width
    float panel_max_width = 1100.0f;
    float panel_max_height = 0.9f;    // of the window's height
    float margin = 64.0f;             // the least room between the panel and the window's edge
    float padding = 32.0f;            // inside the panel
    float panel_radius = 20.0f;
    float border_size = 1.0f;
    // What's on it.
    float item_height = 56.0f;        // a button, box or tab
    float item_min_height = 48.0f;    // nothing to press is shorter
    float item_radius = 12.0f;
    float item_spacing = 8.0f;
    float section_spacing = 24.0f;
    // Text sizes.
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
    float page_ms = 160.0f;           // a page change, its contents sliding page_slide pixels
    float page_slide = 32.0f;
    float focus_ms = 45.0f;           // how quickly the focus ring catches up with a move
    float pulse_ms = 1200.0f;         // one breath of the focus ring
    float press_scale = 0.98f;        // an item held down, springing back over press_ms
    float press_ms = 120.0f;
    float toast_in_ms = 200.0f;       // a notice rising toast_rise pixels as it fades in
    float toast_out_ms = 180.0f;      // and fading out at the end of its time
    float toast_rise = 16.0f;

    // Controller buttons' colours, each 0xRRGGBBAA, by the face the game takes each as: A, B, X
    // and Y (gamepads_faces in gamepads.h). A Nintendo controller's faces are drawn in the text's
    // colour.
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
