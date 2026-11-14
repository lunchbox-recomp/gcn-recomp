// ---- What the Controllers page changes (gamepads.h), each move checked on the ports as the game
// reads them and in ports.txt, then the notices a change makes, then the page itself
// (overlay/overlay_controllers.cpp) and its press-A order. Each runs over the window and guest
// of tools/test_ports_menu_window.cpp's overlay_buttons, which calls them. Read in by
// tools/test_ports.cpp after the frontend, with GCN_OVERLAY.
#include "test_ports_support.h"

namespace {

// The port gamepads_list has `id` on, 1 to 4, 0 for none, or -1 if it doesn't list it.
int listed(uint32_t id) {
    for (const PadDevice& d : gamepads_list().devices)
        if (d.id == id) return d.port + 1;
    return -1;
}

// The last notice queued, or nothing.
std::string last_notice() {
    const std::vector<std::string> n = overlay_notices();
    return n.empty() ? "" : n.back();
}

// A launch from no ports.txt in `dir`, then each of `vs` plugged in with a host frame after
// it. The file's path.
std::string launch_in_menu(Guest& guest, const std::string& dir, std::initializer_list<Virtual*> vs) {
    const std::string file = dir + "/saves/ports.txt";
    fresh_file(file);
    gamepads_init();
    for (Virtual* v : vs) {
        attach(*v);
        host_frame(guest);
    }
    return file;
}

void port_changes(Guest& guest, const std::string& dir) {
    Virtual a{"virtual pad move a"}, b{"virtual pad move b"}, c{"virtual pad move c"};
    const std::string file = launch_in_menu(guest, dir, {&a, &b, &c});
    const PadList start = gamepads_list();
    check(start.devices.size() == 4 && start.devices[0].id == kKeyboard && start.devices[0].name == "Keyboard" &&
              listed(kKeyboard) == 1 && start.devices[1].name == "virtual pad move a" && listed(c.id) == 3 &&
              !start.pinned[0],
          "the list has the keyboard on port 1 first, then each gamepad by its port");

    const bool swapped = gamepads_swap(0, 2);
    const std::string after_swap = read_file(file);
    check(swapped && port(c) == 1 && port(a) == 3 && port(b) == 2 && has(after_swap, line_for(1, c)) &&
              has(after_swap, line_for(3, a)) && gamepads_keyboard_port() == 0,
          "a swap trades two gamepads' ports and lights, ports.txt follows, and the keyboard stays on port 1");
    const bool moved = gamepads_swap(1, 3);
    const std::string after_move = read_file(file);
    check(moved && port(b) == 4 && has(after_move, "\n2 none\n") && has(after_move, line_for(4, b)),
          "a swap with an empty port moves the gamepad there, and ports.txt has none where it was");
    const bool removed = gamepads_unassign(2);
    const std::string after_removal = read_file(file);
    check(removed && port(a) == 0 && listed(a.id) == 0 && player(a) == -1 && has(after_removal, "\n3 none\n") &&
              last_notice() == "virtual pad move a is no longer player 3",
          "a gamepad taken off its port drives none, its light is off, it's listed with none, and ports.txt has none there");
    check(gamepads_assign(a.id, 1) && port(a) == 2 && has(read_file(file), line_for(2, a)),
          "given a port again, it drives that port, and ports.txt names it there");

    const bool unplugged = !read(2).connected;
    const uint64_t unkeyed = file_id(file);
    const bool keys = gamepads_assign(kKeyboard, 2);
    frame();
    check(unplugged && keys && gamepads_keyboard_port() == 2 && listed(kKeyboard) == 3 && read(2).connected &&
              file_id(file) == unkeyed && last_notice() == "Keyboard is now player 3",
          "the keyboard given an empty port plugs it in, and ports.txt doesn't name it");
    gamepads_assign(kKeyboard, 0);
    check(!gamepads_swap(0, 4) && !gamepads_unassign(2) && !gamepads_assign(12345, 2) && !gamepads_assign(kKeyboard, -1),
          "a port out of range, an empty port to clear and a device that isn't here are refused");

    load_controls(dir + "/moves-controls.txt", "A = \"virtual pad nobody\" button 0 port 3\n");
    const uint64_t kept = file_id(file);
    check(gamepads_list().pinned[2] && !gamepads_list().pinned[0] && !gamepads_swap(0, 2) && !gamepads_assign(a.id, 2) &&
              !gamepads_assign(kKeyboard, 2) && !gamepads_unassign(2) && file_id(file) == kept,
          "a port controls.txt pins is listed as pinned, and nothing moves a device onto it or off it");
    no_controls(dir);
    const std::string b_on_4 = line_for(4, b);
    unplug({&a, &b, &c});
    const uint64_t empty = file_id(file);
    check(gamepads_swap(2, 3) && file_id(file) == empty && has(read_file(file), b_on_4),
          "two empty ports swapped leave ports.txt as it was, still naming the gamepad pulled out of one");

    // Each notice lasts 2 s of host time, and the ones so far have gone after that. With the menu
    // closed and no notice, no frame is made for them.
    g_host_ns += 2000000000;
    const long idle = overlay_notice_frames();
    host_frame(guest);
    const bool cleared = overlay_notices().empty() && overlay_notice_frames() == idle;
    Virtual d{"virtual pad notice"};
    const std::string said = dir + "/notices-stderr.txt";
    std::vector<std::string> connected, after_moving, held, expired;
    bool running = false, drawn = false;
    capture(said, [&] {
        attach(d);
        host_frame(guest);
        connected = overlay_notices();
        drawn = overlay_notice_frames() > idle;
        running = !overlay_open() && !pause_requested() && !paused() && guest.passed == guest.asked;
        gamepads_swap(0, 1);
        after_moving = overlay_notices();
        g_host_ns += 1999000000;
        detach(d);
        host_frame(guest);
        held = overlay_notices();
        g_host_ns += 1000000;
        expired = overlay_notices();
    });
    const std::vector<std::string> kConnected = {"virtual pad notice connected as player 1"};
    const std::vector<std::string> kMoved = {kConnected[0], "virtual pad notice is now player 2"};
    const std::vector<std::string> kDropped = {"virtual pad notice disconnected from player 2"};
    const std::vector<std::string> kHeld = {kMoved[0], kMoved[1], kDropped[0]};
    check(cleared && connected == kConnected && after_moving == kMoved,
          "with no notice and the menu closed no frame is made for them; a gamepad connecting queues one notice, and a move one more");
    check(drawn && running, "with the menu closed a notice is laid out over the game, which goes on unpaused");
    check(held == kHeld && expired == kDropped && lines_with(said, "[pads] ") == 3,
          "pulled out it queues one more, each logs one [pads] line, and each lasts 2 s of host time");
    g_host_ns += 2000000000;
    check(overlay_notices().empty(), "after 2 s more none is left");
    gamepads_shutdown();
    gamepads_remember(nullptr);
}

// ---- The Controllers page (overlay/overlay_controllers.cpp) over the same window, and its
// press-A order, checked on the ports and in ports.txt.

// Whether the menu's last frame showed its tabs.
bool tabs_shown() {
    ImGuiContext* context = ImGui::GetCurrentContext();
    for (int i = 0; i < context->TabBars.GetMapSize(); i++) {
        const ImGuiTabBar* bar = context->TabBars.TryGetMapData(i);
        if (bar && bar->CurrFrameVisible == context->FrameCount) return true;
    }
    return false;
}

void controllers_order(Guest& guest, const std::string& dir) {
    Virtual a{"virtual pad order a"}, b{"virtual pad order b"}, c{"virtual pad order c"};
    const std::string file = launch_in_menu(guest, dir, {&a, &b, &c});
    const bool start = port(a) == 1 && port(b) == 2 && port(c) == 3 && gamepads_keyboard_port() == 0;

    // Set player order is the page's first button, below the tabs.
    tap(guest, a, SDL_GAMEPAD_BUTTON_BACK);
    const bool picked = pick_tab(guest, "Controllers");
    for (int i = 0; i < 20; i++) tap(guest, a, SDL_GAMEPAD_BUTTON_DPAD_UP);
    tap(guest, a, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, a, SDL_GAMEPAD_BUTTON_SOUTH);
    check(start && picked && overlay_open() && overlay_controllers_asking() == 0,
          "the Controllers page is laid out in a window with no GL, and its Set player order asks for player 1");

    const long shown = overlay_notice_frames();
    tap(guest, c, SDL_GAMEPAD_BUTTON_SOUTH);
    const std::string first = read_file(file);
    check(listed(c.id) == 1 && listed(a.id) == 3 && listed(b.id) == 2 && overlay_controllers_asking() == 1 &&
              has(first, line_for(1, c)) && has(first, line_for(3, a)) && has(first, line_for(2, b)),
          "the first A to go down takes player 1, the gamepad there takes the port it left, ports.txt says so, and it asks for player 2");
    check(overlay_notice_frames() > shown, "the notices of that move are laid out over the open menu");

    tap(guest, SDL_SCANCODE_X, SDLK_X);
    const bool keyboard = gamepads_keyboard_port() == 1 && listed(kKeyboard) == 2 && listed(b.id) == 2 &&
                          overlay_controllers_asking() == 2;
    tap(guest, c, SDL_GAMEPAD_BUTTON_SOUTH);
    check(keyboard && listed(c.id) == 1 && overlay_controllers_asking() == 2,
          "the keyboard's X takes player 2 beside the gamepad there, and a device already placed is passed over");

    tap(guest, b, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool b_third = listed(b.id) == 3 && listed(a.id) == 2 && overlay_controllers_asking() == 3;
    tap(guest, a, SDL_GAMEPAD_BUTTON_SOUTH);
    const std::string done = read_file(file);
    check(b_third && listed(a.id) == 4 && overlay_controllers_asking() == -1 && overlay_open() &&
              has(done, line_for(1, c)) && has(done, "\n2 none\n") && has(done, line_for(3, b)) && has(done, line_for(4, a)),
          "the next presses take players 3 and 4, the order ends after player 4, the last, and ports.txt names each");

    tap(guest, c, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool again = overlay_controllers_asking() == 0;
    const uint64_t ordered = file_id(file);
    tap(guest, c, SDL_GAMEPAD_BUTTON_BACK);
    check(again && overlay_controllers_asking() == -1 && overlay_open() && file_id(file) == ordered && listed(c.id) == 1,
          "Set player order is focused again after, and Back ends the order with nothing moved, the menu still open");
    tap(guest, c, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool b_asked = overlay_controllers_asking() == 0;
    tap(guest, c, SDL_GAMEPAD_BUTTON_EAST);
    check(b_asked && overlay_controllers_asking() == -1 && overlay_open() && file_id(file) == ordered,
          "B ends the order with nothing moved, the menu still open");
    tap(guest, c, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool f1_asked = overlay_controllers_asking() == 0;
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    g_host_ns += 1000000000;  // past the close's fade, which still draws the order
    host_frame(guest);
    const bool f1_closed = !overlay_open() && overlay_controllers_asking() == -1;
    tap(guest, SDL_SCANCODE_F1, SDLK_F1);
    check(f1_asked && f1_closed && overlay_open() && overlay_controllers_asking() == -1 && tabs_shown(),
          "F1 during the order closes the menu and ends the order, and opened again the menu shows its tabs, not the prompt");
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(!overlay_open() && port(c) == 1 && port(b) == 3 && port(a) == 4,
          "with the menu closed the game reads each gamepad on its new port, and each player light shows it");

    // Port 3 pinned with no gamepad on it.
    gamepads_unassign(2);
    load_controls(dir + "/order-controls.txt", "A = \"virtual pad nobody\" button 0 port 3\n");
    tap(guest, a, SDL_GAMEPAD_BUTTON_BACK);
    overlay_controllers_order();
    tap(guest, c, SDL_GAMEPAD_BUTTON_SOUTH);
    tap(guest, a, SDL_GAMEPAD_BUTTON_SOUTH);
    const bool skips = listed(c.id) == 1 && listed(a.id) == 2 && listed(b.id) == 0 && overlay_controllers_asking() == 3;
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    check(skips && overlay_controllers_asking() == -1 && overlay_open(),
          "the order passes over a pinned port, asking for player 4 after player 2, and Escape ends it with the menu open");
    tap(guest, a, SDL_GAMEPAD_BUTTON_BACK);
    no_controls(dir);
    unplug({&a, &b, &c});
    gamepads_shutdown();
    gamepads_remember(nullptr);
}

// A two-gamepad order with a Switch Pro pad, then the page's Swap and Remove pressed, with the
// keyboard on a player beside them.
void controllers_buttons(Guest& guest, const std::string& dir) {
    Virtual pad{"virtual pad page"}, nintendo{"virtual switch pro page"};
    nintendo.vendor = 0x057e;
    nintendo.product = 0x2009;
    const std::string file = launch_in_menu(guest, dir, {&pad, &nintendo});
    const bool start = listed(pad.id) == 1 && listed(nintendo.id) == 2;
    tap(guest, pad, SDL_GAMEPAD_BUTTON_BACK);
    const bool picked = pick_tab(guest, "Controllers");
    overlay_controllers_order();
    tap(guest, nintendo, SDL_GAMEPAD_BUTTON_EAST);
    check(start && picked && listed(nintendo.id) == 1 && listed(pad.id) == 2 && overlay_controllers_asking() == 1,
          "a Switch Pro pad's east, the button labeled A, takes player 1 in the order");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(listed(pad.id) == 2 && overlay_controllers_asking() == -1 && overlay_open(),
          "with two gamepads the order ends at player 2, once both are placed");

    // Set player order is focused after it, and down from there is player 1's Swap.
    detach(nintendo);
    host_frame(guest);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(listed(pad.id) == 1 && gamepads_keyboard_port() == 1 && has(read_file(file), line_for(1, pad)),
          "the page's Swap trades players 1 and 2, the keyboard too: the gamepad ends on player 1 and the keyboard on 2");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const std::string back = read_file(file);
    check(listed(pad.id) == 2 && gamepads_keyboard_port() == 0 && has(back, line_for(2, pad)) && has(back, "\n1 none\n"),
          "pressed again with only the keyboard on player 2, it trades them back, and ports.txt has the gamepad on 2 and none on 1");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    const std::string removed = read_file(file);
    check(listed(pad.id) == 0 && gamepads_keyboard_port() == 0 && has(removed, "\n2 none\n"),
          "player 2's Remove, right of its Swap, takes its gamepad off, and ports.txt has none there");
    const uint64_t kept = file_id(file);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    tap(guest, pad, SDL_GAMEPAD_BUTTON_SOUTH);
    check(overlay_asking() != OverlayAsk::none && gamepads_keyboard_port() == 0 && file_id(file) == kept,
          "with no gamepad on any player, each Swap and Remove is off, so down from player 2 is the row below the page");
    tap(guest, pad, SDL_GAMEPAD_BUTTON_EAST);
    overlay_close();
    host_frame(guest);
    unplug({&pad});
    gamepads_shutdown();
    gamepads_remember(nullptr);
}

// A gamepad on no player, left out as it came with all four taken, takes an occupied player in
// the order, and the gamepad there goes to none.
void order_left_out(Guest& guest, const std::string& dir) {
    Virtual a{"virtual pad four a"}, b{"virtual pad four b"}, c{"virtual pad four c"}, d{"virtual pad four d"},
        e{"virtual pad fifth"};
    const std::string file = launch_in_menu(guest, dir, {&a, &b, &c, &d, &e});
    check(listed(e.id) == 0 &&
              last_notice() == "virtual pad fifth has no player: all four are taken. The menu's Controllers page can give it one.",
          "a fifth gamepad gets no player, and its notice says the Controllers page can give it one");
    tap(guest, a, SDL_GAMEPAD_BUTTON_BACK);
    overlay_controllers_order();
    tap(guest, e, SDL_GAMEPAD_BUTTON_SOUTH);
    check(listed(e.id) == 1 && listed(a.id) == 0 && has(read_file(file), line_for(1, e)) && overlay_controllers_asking() == 1,
          "its A in the order takes player 1, the gamepad there is listed on none, and ports.txt names the newcomer");
    tap(guest, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    overlay_close();
    host_frame(guest);
    unplug({&a, &b, &c, &d, &e});
    gamepads_shutdown();
    gamepads_remember(nullptr);
}

}  // namespace
