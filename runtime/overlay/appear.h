// How the menu comes and goes (overlay.h), on the host clock (motion.h). As it opens, the
// backdrop dims the game over the theme's backdrop_ms, out-quad, and the panel fades in and grows
// from the theme's open_scale over its open_ms, out-back, with its contents following
// contents_delay_ms behind and whole with it. As it closes, the panel, its contents and the
// backdrop fade out over close_ms, in-quad, the panel shrinking to close_scale. Opened or closed
// mid-way, each starts from where it was. With motion off each change is whole at once.
#pragma once
#include "overlay/theme.h"
#include <cstdint>

struct ImDrawData;
struct ImDrawList;

void appear_motion(bool on);
void appear_open(const OverlayTheme& theme, uint64_t now);
void appear_close(const OverlayTheme& theme, uint64_t now);
// Closed, and still fading out.
bool appear_closing(uint64_t now);
// Anything still moving, the backdrop included.
bool appear_moving(uint64_t now);
// Open, with the panel and its contents whole.
bool appear_shown(uint64_t now);
// From 0 to 1: the backdrop, as a share of the theme's alpha for it; the panel's alpha and its
// contents'. And the panel's scale.
float appear_backdrop(uint64_t now);
float appear_panel_alpha(uint64_t now);
float appear_contents_alpha(uint64_t now);
float appear_scale(uint64_t now);
// After ImGui::Render: every list of `d` but `skip`, the backdrop's, scaled by `scale` about
// (cx, cy), its clip rects too, and faded: `panel`'s first `panel_vtx` vertices, the panel's
// own, by `panel_alpha`, and the rest by `contents_alpha`.
void appear_apply(ImDrawData* d, const ImDrawList* skip, const ImDrawList* panel, int panel_vtx, float cx, float cy,
                  float scale, float panel_alpha, float contents_alpha);
