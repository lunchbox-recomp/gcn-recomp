// ---- The Buttons view (overlay/overlay_bindings.cpp) and its press-to-bind, over the window and
// guest of tools/test_ports_menu_window.cpp's overlay_buttons, which calls it, checked on the
// ports as the game reads them and in pads.txt. Read in by tools/test_ports.cpp after the
// frontend, with GCN_OVERLAY.
#include "test_ports_support.h"
#include "overlay/footer.h"  // the hints the footer drew
#include "overlay/state.h"   // the menu's window, to change its height
#include "overlay/tabs.h"    // where the header laid out its first pill

namespace {

// Whether the item with the focus is the one labelled `label` in the menu's window.
bool focused(const std::string& label) {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    return g.NavWindow && g.NavId == g.NavWindow->GetID(label.c_str());
}

// The label of the view's first item, the first gamepad listed.
std::string first_pad() {
    for (const PadDevice& d : gamepads_list().devices)
        if (d.id != kKeyboard)
            return (d.port >= 0 ? "Player " + std::to_string(d.port + 1) + ": " + d.name : d.name + " (no player)") +
                   "##" + std::to_string(d.id);
    return "";
}

// The room under the footer's hints, as the menu's last frame laid them out, inside the padding of
// `panel`, the menu's.
float under_hints(const ImGuiWindow* panel) {
    float f[4];
    footer_test_hints_rect(f);
    return panel->Pos.y + panel->Size.y - ImGui::GetStyle().WindowPadding.y - f[3];
}

// Whether the footer's hints and the item with the focus are whole on the menu's panel, as its
// last frame laid them out, the item inside the region it's in too, which the Buttons view
// scrolls. The hints are inside the panel's padding, but for the part of a pixel Dear ImGui drops
// from the height it sizes the panel to, and the panel has nothing to scroll, either way. False
// with no item laid out with the focus.
bool on_panel() {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* panel = ImGui::FindWindowByName("Test title");
    if (!panel || !g.NavWindow || !g.NavIdIsAlive) return false;
    float f[4];
    footer_test_hints_rect(f);
    const ImRect item = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]);
    return panel->InnerClipRect.Contains(ImRect(f[0], f[1], f[2], f[3])) && under_hints(panel) > -1.0f &&
           panel->ScrollMax.x == 0.0f && panel->ScrollMax.y == 0.0f && panel->InnerClipRect.Contains(item) &&
           g.NavWindow->InnerClipRect.Contains(item);
}

// The focus moved by `move` until it stays on one item, at most 60 times, on_panel after each.
// How many moves went to another item, or -1 for one after which it wasn't on_panel.
int walk(const std::function<void()>& move) {
    for (int n = 0; n < 60; n++) {
        const ImGuiID was = ImGui::GetCurrentContext()->NavId;
        move();
        if (!on_panel()) return -1;
        if (ImGui::GetCurrentContext()->NavId == was) return n;
    }
    return -1;
}

// Down the Buttons view from its first gamepad to Done and back up, a press at a time, on the
// D-pad and on the keyboard's arrows, the focus and the footer's hints on the panel at each
// (on_panel). The view has no wrap: down at Done, or up at the top, stays there. The left stick
// moves no focus, as Dear ImGui has it: it scrolls the window with the focus, here the region the
// view's rows scroll in, under a header and a footer that stay where they are, and the D-pad's
// next press puts the focus on a row in view. Then a press-to-bind on the last row, put up and
// cancelled from the top, takes the focus back to that row, scrolled into view.
void view_scrolls(Guest& guest, const Virtual& pad) {
    const struct {
        const char* by;
        std::function<void()> down, up;
    } kWays[] = {
        {"the D-pad", [&] { tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN); },
         [&] { tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP); }},
        {"the arrows", [&] { tap(guest, SDL_SCANCODE_DOWN, SDLK_DOWN); }, [&] { tap(guest, SDL_SCANCODE_UP, SDLK_UP); }},
    };
    for (const auto& way : kWays) {
        const int down = walk(way.down);
        const bool done = focused("Done");
        const int up = walk(way.up);
        check(down > 20 && done && up > 20 && focused(first_pad()),
              (std::string("down the Buttons view to Done and back up on ") + way.by +
               ", the item with the focus and the footer's hints stay whole on the panel at each move")
                  .c_str());
    }
    // The stick held down until the region the focus is in has scrolled to its foot. Dear ImGui
    // scrolls it by the frame's time, so it's held for as many frames as that takes.
    const ImGuiWindow* region = ImGui::GetCurrentContext()->NavWindow;
    float x = 0.0f, header = 0.0f, still = -1.0f;
    tabs_test_pill(0, x, header);
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFTY, 32767);
    for (int f = 0; f < 2000 && region && (region->ScrollMax.y <= 0.0f || region->Scroll.y < region->ScrollMax.y); f++)
        host_frame(guest);
    set_axis(pad, SDL_GAMEPAD_AXIS_LEFTY, 0);
    host_frame(guest);
    float hints[4];
    footer_test_hints_rect(hints);
    const ImGuiWindow* panel = ImGui::FindWindowByName("Test title");
    const bool scrolled = region && region->Scroll.y > 0.0f && focused(first_pad()) && panel &&
                          panel->InnerClipRect.Contains(ImRect(hints[0], hints[1], hints[2], hints[3]));
    tabs_test_pill(0, x, still);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    check(scrolled && still == header && on_panel() && !focused(first_pad()),
          "the left stick scrolls the Buttons view's rows under a still header and footer, and the D-pad then puts "
          "the focus on a row in view");
    for (int i = 0; i < 40; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    overlay_bindings_listen(PadControl::CStickY);
    host_frame(guest);
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    host_frame(guest);
    float last[4];
    overlay_bindings_test_last_row(last);
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImRect item = g.NavWindow ? ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[g.NavLayer]) : ImRect();
    check(on_panel() && item.Min.y == last[1] && item.Max.y == last[3],
          "press-to-bind on the last row cancelled from the top of the view takes the focus to that row, in view");
    for (int i = 0; i < 40; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
}

// The window grown to 1440 lines with the focus on Done at the view's foot, shrunk to 480, grown
// to 2160, then back to 960. From 480 to 2160 the view at 480 is shorter than the region can be
// at 2160, so the region takes the view's last height at the new scale. The frame laid out as the
// height changes has the panel at the size the frame before gave it, as any page has. From the
// next on the footer's hints and Done are on the panel (on_panel), which doesn't scroll, and the
// header stays where it is.
void view_resized(Guest& guest, const Virtual& pad) {
    for (int i = 0; i < 40 && !focused("Done"); i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    const bool done = focused("Done");
    int w = 0, h = 0;
    SDL_GetWindowSize(overlay_window(), &w, &h);
    bool settled = true;
    for (const int height : {1440, 480, 2160, h}) {
        SDL_SetWindowSize(overlay_window(), w, height);
        host_frame(guest);
        float x = 0.0f, header = 0.0f;
        for (int f = 1; f < 4; f++) {
            host_frame(guest);
            float y = 0.0f;
            tabs_test_pill(0, x, y);
            if (f == 1) header = y;
            settled = settled && on_panel() && y == header && focused("Done");
        }
    }
    check(done && settled,
          "the window's height changed with the Buttons view up, from the next frame the footer's hints and the "
          "focused row are on the panel, which doesn't scroll, under a still header");
    for (int i = 0; i < 40; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
}

void bindings_page_checks(Guest& guest, const std::string& dir) {
    const std::string file = dir + "/saves/pads.txt";
    gamepads_bindings_file(file.c_str());
    Virtual pad{"virtual pad buttons view"}, other{"virtual pad buttons other"};
    launch_in_menu(guest, dir, {&pad, &other});

    // Change buttons is right of Set player order, the page's first button.
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    pick_page(guest, "Controllers");
    const std::string page_hints = footer_test_hints();
    for (int i = 0; i < 20; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(overlay_bindings_shown() && overlay_open() && !overlay_bindings_listening(),
          "Change buttons, right of Set player order, puts up the Buttons view");
    const std::string view_hints = footer_test_hints();
    host_frame(guest);
    check(focused(first_pad()), "the view opens with its first gamepad focused");
    const ImGuiWindow* region = ImGui::GetCurrentContext()->NavWindow;
    check(region && region->ScrollbarY && footer_test_rule_right() == region->WorkRect.Max.x,
          "the footer's rule under the Buttons view ends where the rows' rules do, short of their scroll bar");
    view_scrolls(guest, pad);
    view_resized(guest, pad);

    // Down past the two gamepads at the view's top is A's row. The A that starts press-to-bind
    // starts it as the A goes down, so it's still down as it starts, and passed over.
    for (int i = 0; i < 20; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    for (int i = 0; i < 2; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
    host_frame(guest);
    const bool on_press = overlay_bindings_listening();
    set_button(pad, SDL_GAMEPAD_BUTTON_SOUTH, false);
    host_frame(guest);
    const bool passed_over = on_press && overlay_bindings_listening();
    const std::string listening_hints = footer_test_hints();
    check(page_hints == "A Select, B Close, L R Page" && view_hints == "A Select, B Back",
          "the footer hints B Close on a page and B Back in the Buttons view, as B does there");
    check(listening_hints == "Back Cancel, Esc Cancel",
          "while press-to-bind listens the footer hints only its way out, Back or Escape, and no page turn");
    tap(guest, other, SDL_GAMEPAD_BUTTON_NORTH);
    const bool other_pad = overlay_bindings_listening();
    tap(guest, pad, SDL_GAMEPAD_BUTTON_WEST);
    const std::string text = read_file(file);
    check(passed_over && other_pad && !overlay_bindings_listening() && overlay_bindings_shown() &&
              has(text, "A = \"virtual pad buttons view\" gamepad west\n") &&
              has(text, "X = \"virtual pad buttons view\" gamepad south\n"),
          "press-to-bind passes over the A held as it starts and another gamepad's press, then takes west for A, "
          "X takes south, and pads.txt says so");
    check(has(overlay_bindings_said(), "A is now ") && has(overlay_bindings_said(), " X is now "),
          "the view says what A and X are now");

    overlay_bindings_listen(PadControl::B);
    g_host_ns += kBindListenNs;
    host_frame(guest);
    check(!overlay_bindings_listening() && has(overlay_bindings_said(), "Nothing was pressed in 5 seconds, so B is still "),
          "with nothing pressed for 5 s it gives up and says so");

    const uint64_t kept = file_id(file);
    overlay_bindings_listen(PadControl::B);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    const bool back = !overlay_bindings_listening() && overlay_open() && overlay_bindings_shown();
    overlay_bindings_listen(PadControl::B);
    tap(guest, other, SDL_GAMEPAD_BUTTON_BACK);
    const bool other_back = !overlay_bindings_listening() && overlay_open();
    overlay_bindings_listen(PadControl::B);
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(back && other_back && !overlay_bindings_listening() && overlay_open() && overlay_bindings_shown() &&
              file_id(file) == kept && has(overlay_bindings_said(), "B is still "),
          "Back on its gamepad or another, or Escape, cancels it with the menu still open and nothing saved");

    // Guide cancels as Back does, on its gamepad or another, and on one with Guide and no Back,
    // whose Guide would otherwise open and close the menu, it cancels and does nothing more.
    overlay_bindings_listen(PadControl::B);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_GUIDE);
    const bool guide = !overlay_bindings_listening() && overlay_open();
    overlay_bindings_listen(PadControl::B);
    tap(guest, other, SDL_GAMEPAD_BUTTON_GUIDE);
    const bool other_guide = !overlay_bindings_listening() && overlay_open();
    Virtual home{"home key pad buttons"};
    home.button_mask = 1u << SDL_GAMEPAD_BUTTON_SOUTH | 1u << SDL_GAMEPAD_BUTTON_EAST | 1u << SDL_GAMEPAD_BUTTON_WEST |
                       1u << SDL_GAMEPAD_BUTTON_NORTH | 1u << SDL_GAMEPAD_BUTTON_START | 1u << SDL_GAMEPAD_BUTTON_GUIDE;
    attach(home);
    host_frame(guest);
    overlay_bindings_open(home.id);
    overlay_bindings_listen(PadControl::B);
    host_frame(guest);
    const std::string home_hints = footer_test_hints();
    tap(guest, home, SDL_GAMEPAD_BUTTON_GUIDE);
    check(guide && other_guide && !overlay_bindings_listening() && overlay_open() && overlay_bindings_shown() &&
              file_id(file) == kept && has(overlay_bindings_said(), "B is still ") &&
              !has(overlay_bindings_said(), "seconds"),
          "Guide on its gamepad or another cancels it too, and on a gamepad with no Back it doesn't close the menu");
    check(home_hints == "Guide Cancel, Esc Cancel", "listening to a gamepad with Guide and no Back, the footer hints Guide");
    unplug({&home});

    // A key pressed as it listens, the footer still draws the listening gamepad's own Back, by
    // its family's name, never a keycap.
    // Before that, the view's rows draw each pad's own glyphs in its family's colours: an Xbox
    // pad's A and B discs, its RB for Z and its LT for L as pills on the item's colour with its
    // edge, and a PlayStation pad's A as a Cross disc with the cross on it.
    Virtual xbox{"xbox pad buttons view"}, ps{"playstation pad buttons view"};
    xbox.vendor = 0x045e;
    xbox.product = 0x02ea;
    ps.vendor = 0x054c;
    ps.product = 0x0ce6;
    attach(xbox);
    attach(ps);
    host_frame(guest);
    const OverlayTheme theme = overlay_theme_default();
    overlay_bindings_open(ps.id);
    host_frame(guest);
    const std::vector<OverlayGlyph> ps_rows = overlay_bindings_test_glyphs();
    overlay_bindings_open(xbox.id);
    host_frame(guest);
    const std::vector<OverlayGlyph> rows = overlay_bindings_test_glyphs();
    const auto pill = [&](const OverlayGlyph& g, const char* label) {
        return g.label == label && g.shape == OverlayGlyphShape::pill && g.fill == theme.item && g.edge == theme.border;
    };
    check(rows.size() == 12 && rows[0].label == "A" && rows[0].fill == theme.xbox.a &&
              rows[0].shape == OverlayGlyphShape::circle && rows[1].label == "B" && rows[1].fill == theme.xbox.b &&
              pill(rows[(int)PadControl::Z], "RB") && pill(rows[(int)PadControl::L], "LT") &&
              pill(rows[(int)PadControl::R], "RT"),
          "the Buttons view draws an Xbox pad's A and B in its colours and RB, LT and RT as edged pills");
    check(ps_rows.size() == 12 && ps_rows[0].label == "Cross" && ps_rows[0].mark == OverlayGlyphMark::cross &&
              ps_rows[0].fill == theme.playstation.a && pill(ps_rows[(int)PadControl::L], "L2"),
          "the Buttons view draws a PlayStation pad's A as a Cross disc with its cross, and L as L2");
    // The body's size at this window's scale, rounded to a pixel as Dear ImGui rounds a font's.
    const float body = std::round(theme.body_size * ImGui::GetStyle().FontScaleMain) * 1.3f;
    check(std::abs(overlay_bindings_test_glyph_height() - body) < 0.01f,
          "the Buttons view's glyphs are drawn at the body's size, as its rows' names are, not the footer's");
    unplug({&ps});
    overlay_bindings_listen(PadControl::B);
    tap(guest, SDL_SCANCODE_K, SDLK_K);
    const std::vector<OverlayGlyph> keyed = footer_test_glyphs();
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(keyed.size() == 2 && keyed[0].label == "View" && keyed[0].shape == OverlayGlyphShape::pill &&
              keyed[1].label == "Esc" && keyed[1].shape == OverlayGlyphShape::keycap && !overlay_bindings_listening(),
          "listening after a key, the footer draws the Xbox pad's own View, not a Back keycap, and Escape's");
    unplug({&xbox});

    // A GameCube pad on Nintendo's adapter has no Back or Guide, and each of its buttons is the
    // binding's, so the footer hints no button of it: Escape, or the wait running out.
    Virtual adapter{"adapter pad buttons"};
    adapter.button_mask = kCubeButtons | kClicks;
    attach(adapter);
    host_frame(guest);
    overlay_bindings_open(adapter.id);
    overlay_bindings_listen(PadControl::B);
    host_frame(guest);
    const std::string adapter_hints = footer_test_hints();
    const size_t adapter_glyphs = footer_test_glyphs().size();
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(adapter_hints == "Esc Cancel, Wait Cancel" && adapter_glyphs == 1 &&
              !overlay_bindings_listening() && file_id(file) == kept,
          "listening to a GameCube pad on the adapter, the footer hints Escape and the wait, and Escape cancels");
    unplug({&adapter});

    // Press-to-bind's prompt, the gamepad gone as it waits, and a press that changes nothing.
    Virtual gone{"virtual pad buttons gone"};
    plug({&gone});
    host_frame(guest);
    overlay_bindings_open(gone.id);
    overlay_bindings_listen(PadControl::Y);
    host_frame(guest);
    detach(gone);
    host_frame(guest);
    check(!overlay_bindings_listening() && overlay_bindings_said() == "virtual pad buttons gone was disconnected, so nothing changed.",
          "a gamepad pulled out as it waits ends it, saying the gamepad's name");
    overlay_bindings_open(pad.id);
    overlay_bindings_listen(PadControl::A);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_WEST);
    check(file_id(file) == kept && has(overlay_bindings_said(), "A is still "),
          "the button A has already, pressed for A, changes nothing and saves nothing");

    // One way of a stick axis on a button, and all of it on a stick control, don't swap: the
    // view says the stick drives both. Then each is set back as it was.
    PadBinding before;
    gamepads_binding(pad.id, before);
    auto push_for = [&](PadControl c, int axis, Sint16 value) {
        overlay_bindings_listen(c);
        set_axis(pad, axis, value);
        host_frame(guest);
        set_axis(pad, axis, 0);
        host_frame(guest);
    };
    push_for(PadControl::DUp, SDL_GAMEPAD_AXIS_LEFTY, -32767);
    const std::string up = overlay_bindings_said();
    push_for(PadControl::StickX, SDL_GAMEPAD_AXIS_LEFTY, 32767);
    const std::string whole = overlay_bindings_said();
    push_for(PadControl::StickX, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    push_for(PadControl::StickY, SDL_GAMEPAD_AXIS_LEFTY, -32767);
    overlay_bindings_listen(PadControl::DUp);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    PadBinding after;
    check(up == "D-pad up is now Left stick up. Control stick up and down still moves with it." &&
              has(whole, "Control stick left and right is now Left stick.") &&
              has(whole, " Left stick up still presses D-pad up too.") && gamepads_binding(pad.id, after) &&
              after == before,
          "a stick's up on D-pad up says the control stick still moves with it, the stick's y on the control stick "
          "says it still presses D-pad up, and each sets back");

    // Two gamepads pads.txt can't tell apart both take a change, and the view says so.
    Virtual twin{"virtual pad buttons view"};
    plug({&twin});
    host_frame(guest);
    overlay_bindings_listen(PadControl::Start);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_NORTH);
    check(has(overlay_bindings_said(), "Player 3's controller has it too"),
          "a change one gamepad shares with another it can't be told from says which player has it too");
    unplug({&twin});

    // Test rumble asks a GameCube pad whose flag says it has none, as the game's rumble does.
    Virtual cube{"gc rumble pad buttons"};
    cube.vendor = 0x057e;
    cube.product = 0x0337;
    attach(cube);
    SDL_Joystick* held = SDL_OpenJoystick(cube.id);
    SDL_SetBooleanProperty(SDL_GetJoystickProperties(held), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
    host_frame(guest);
    overlay_bindings_open(cube.id);
    host_frame(guest);
    for (int i = 0; i < 40; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(cube.rumbles == 1 && overlay_bindings_said() == "Rumble sent to gc rumble pad buttons for 1 second.",
          "Test rumble, above Done, rumbles a GameCube pad SDL says has none, and says it was sent");
    SDL_CloseJoystick(held);
    unplug({&cube});

    // A left Joy-Con alone that has a trigger is held upright, so its shoulder is L and its
    // trigger ZL (pad_glyph.h).
    Virtual joycon{"joy-con pad buttons"};
    joycon.vendor = 0x057e;
    joycon.product = 0x2006;
    plug({&joycon});
    overlay_bindings_open(joycon.id);
    overlay_bindings_listen(PadControl::L);
    tap(guest, joycon, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    const std::string shoulder = overlay_bindings_said();
    overlay_bindings_listen(PadControl::L);
    tap(guest, joycon, SDL_GAMEPAD_BUTTON_BACK);
    check(shoulder == "L is now L." && overlay_bindings_said() == "L is still L.",
          "an upright left Joy-Con's shoulder, bound to L, is named L");
    unplug({&joycon});

    // A Steam Deck has misc1, so its first left paddle is L4, and the first Steam Controller has
    // none, so its is the left grip (pad_glyph.h). Both have triggers, so a view that mixes up or
    // drops what it reads of the trigger and misc1 names one of them wrong.
    Virtual deck{"steam deck pad buttons"}, grips{"steam grips pad buttons"};
    deck.vendor = grips.vendor = 0x28de;
    deck.product = 0x1205;
    grips.product = 0x1102;
    grips.button_mask = ((1u << SDL_GAMEPAD_BUTTON_COUNT) - 1) & ~(1u << SDL_GAMEPAD_BUTTON_MISC1);
    std::string paddle[2];
    Virtual* steam[2] = {&deck, &grips};
    for (int i = 0; i < 2; i++) {
        plug({steam[i]});
        overlay_bindings_open(steam[i]->id);
        overlay_bindings_listen(PadControl::L);
        tap(guest, *steam[i], SDL_GAMEPAD_BUTTON_LEFT_PADDLE1);
        paddle[i] = overlay_bindings_said();
        unplug({steam[i]});
    }
    check(paddle[0] == "L is now L4." && paddle[1] == "L is now Left grip.",
          "a Steam Deck's first left paddle is L4, with misc1, and the first Steam Controller's its "
          "left grip, with none");
    overlay_bindings_open(pad.id);

    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    check(!overlay_bindings_shown() && overlay_open(), "B in the view goes back to the Controllers page");
    host_frame(guest);
    const bool on_change = focused("Change buttons");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(on_change && overlay_bindings_shown(),
          "back from the view Change buttons has the focus, and A puts the view up again at once");
    overlay_bindings_open(pad.id);
    overlay_bindings_listen(PadControl::Z);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    g_host_ns += 1000000000;  // past the close's fade, which still draws the view
    host_frame(guest);
    check(!overlay_open() && !overlay_bindings_listening() && !overlay_bindings_shown(),
          "F1 while it listens closes the menu, and the view and the listening end once it's faded");
    set_button(pad, SDL_GAMEPAD_BUTTON_WEST, true);
    host_frame(guest);
    const bool a_in_game = read(0).buttons == PAD_A;
    set_button(pad, SDL_GAMEPAD_BUTTON_WEST, false);
    host_frame(guest);
    check(a_in_game, "with the menu closed, the game reads the gamepad's west as A");

    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    host_frame(guest);
    check(overlay_open() && focused("Change buttons"),
          "closed from the view and opened again, the Controllers page has Change buttons focused");

    // Escape and F1 in one frame: the view goes back to the page, which the close's fade draws
    // with the focus where it was, and the menu opened again has Change buttons focused.
    overlay_bindings_open(pad.id);
    host_frame(guest);
    push_key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, true);
    push_key(SDL_SCANCODE_F1, SDLK_F1, true);
    host_frame(guest);
    push_key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, false);
    push_key(SDL_SCANCODE_F1, SDLK_F1, false);
    host_frame(guest);
    const bool went_back = !overlay_open() && !overlay_bindings_shown() && !focused("Change buttons");
    g_host_ns += 1000000000;  // past the close's fade
    host_frame(guest);
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    host_frame(guest);
    check(went_back && overlay_open() && focused("Change buttons"),
          "back from the view as the menu closes, the fade moves no focus, and opened again Change buttons has it");

    // Escape and F1 in one frame: the cancel picks the row the focus goes back to, and the menu
    // closes before a draw takes it there. The view opened again has no row picked for it.
    overlay_bindings_open(pad.id);
    overlay_bindings_listen(PadControl::B);
    push_key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, true);
    push_key(SDL_SCANCODE_F1, SDLK_F1, true);
    host_frame(guest);
    push_key(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, false);
    push_key(SDL_SCANCODE_F1, SDLK_F1, false);
    host_frame(guest);
    const bool closed = !overlay_open();
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    overlay_bindings_open(pad.id);
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(closed && overlay_bindings_shown() && !overlay_bindings_listening(),
          "a row press-to-bind ended on as the menu closed isn't picked when the view opens again");
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);

    // A save that fails says the change lasts until the game quits: a folder that can't be
    // written, with no pads.txt in it, so the file set anew reads as the map and north is a
    // change for Start.
    const std::string locked = dir + "/locked";
    mkdir(locked.c_str(), 0555);
    gamepads_bindings_file((locked + "/pads.txt").c_str());
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    overlay_bindings_open(pad.id);
    overlay_bindings_listen(PadControl::Start);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_NORTH);
    check(has(overlay_bindings_said(), "Start is now ") && has(overlay_bindings_said(), "until the game quits"),
          "a change pads.txt couldn't take says it lasts until the game quits");
    // Trigger click, above Reset to default, taken with A, one step right, and let go with A.
    for (int i = 0; i < 40; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    PadBinding clicked;
    check(gamepads_binding(pad.id, clicked) && clicked.trigger_click == 30473 &&
              overlay_bindings_said() == "Trigger click is now 93%. pads.txt isn't saved, so this lasts until the "
                                         "game quits.",
          "a slider moved a step and let go says what it's now, and that pads.txt isn't saved");
    for (int i = 0; i < 40; i++) tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(overlay_bindings_said() == "virtual pad buttons view has its default buttons, dead zones and trigger click "
                                     "again. pads.txt isn't saved, so this lasts until the game quits.",
          "Reset to default, right of Test rumble, says what it put back, and that pads.txt isn't saved");
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    chmod(locked.c_str(), 0755);

    load_controls(dir + "/view-controls.txt", "A = \"virtual pad buttons other\" button 0\n");
    check(!overlay_bindings_open(other.id), "a gamepad controls.txt binds can't be put up in the view");
    no_controls(dir);
    unplug({&pad, &other});
    gamepads_shutdown();
    gamepads_bindings_file(nullptr);
    gamepads_remember(nullptr);
}

}  // namespace
