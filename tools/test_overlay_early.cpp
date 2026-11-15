// A game's file in test_ports, for --init-order (tools/test_ports_init_order.cpp): its static
// initializer sets the theme, adds a page, sets a telemetry default and, with the overlay, sets
// the menu's sound theme, as a game's SOURCES may.
// CMakeLists.txt lists it ahead of the overlay's files and telemetry.cpp, as the desktop
// executable has a game's, so on macOS, which runs initializers in link order, it runs before
// theirs. Compiled on its own, not read in by tools/test_ports.cpp.
#include "overlay/overlay.h"
#include "telemetry.h"
#include "ui_sound.h"
#include <cstdint>

extern const uint32_t kEarlyPanel = 0x203040F0;
extern const float kEarlyGain = 0.25f;  // telemetry's early_gain, a key the module doesn't know
extern const float kEarlyPeak = -20.0f;  // the menu sounds' peak_dbfs, not the default's
int g_early_draws;

namespace {

void early_page(void*) { g_early_draws++; }

struct Early {
    Early() {
        OverlayTheme theme = overlay_theme_default();
        theme.panel = kEarlyPanel;
        overlay_set_theme(theme);
        overlay_add_page("Early", early_page, nullptr);
        telemetry::set_default("early_gain", kEarlyGain);
#ifdef GCN_OVERLAY  // ui_sound.cpp is linked with the overlay only
        UiSoundTheme sound = ui_sound_theme_default();
        sound.peak_dbfs = kEarlyPeak;
        ui_sound_set_theme(sound);
#endif
    }
} g_early;

}  // namespace
