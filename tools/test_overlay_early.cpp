// A game's file in test_ports, for --init-order (tools/test_ports_init_order.cpp): its static
// initializer sets the theme and adds a page, as a game's SOURCES may. CMakeLists.txt lists it
// ahead of the overlay's files, as the desktop executable has a game's, so on macOS, which runs
// initializers in link order, it runs before theirs. Compiled on its own, not read in by
// tools/test_ports.cpp.
#include "overlay/overlay.h"
#include <cstdint>

extern const uint32_t kEarlyPanel = 0x203040F0;
int g_early_draws;

namespace {

void early_page(void*) { g_early_draws++; }

struct Early {
    Early() {
        OverlayTheme theme = overlay_theme_default();
        theme.panel = kEarlyPanel;
        overlay_set_theme(theme);
        overlay_add_page("Early", early_page, nullptr);
    }
} g_early;

}  // namespace
