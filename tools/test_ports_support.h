// What more than one area of the gamepad port checks uses: the checks' count, SDL's virtual
// gamepads, a frame of the frontend and a port as the guest reads it, the work dir's files and
// stderr, and with GCN_OVERLAY a thread for the guest and the menu's keys and tabs.
// tools/test_ports_support.cpp defines them. tools/test_ports.cpp reads this, that and each
// area's file in after the frontend it compiles in, so all of them see the frontend's statics.
#pragma once
#include <climits>
#include <condition_variable>
#include <functional>
#include <initializer_list>
#include <string>
#ifdef GCN_OVERLAY
#include "imgui.h"  // what the menu's navigation reads of the gamepads
#ifdef GCN_OVERLAY_TEST
#include "imgui_internal.h"  // inspect and select the registered page's tab
#endif
#endif

namespace {

void check(bool ok, const char* what);
// SDL let go, then the mode's last line, its checks passed. The mode's exit code.
int finish(const char* what);
// The mode's last line from an at-exit function, which ends the process with the mode's code.
[[noreturn]] void finish_now(const char* what);

const int kSdlIndex = INT_MIN;  // attach(): keep the player index SDL gives

struct Virtual {
    const char* name;
    bool gamepad = true;
    SDL_JoystickID id = 0;
    int rumbles = 0;  // rumbles started on it
    int stops = 0;    // rumbles stopped on it
    int player = -1;  // the player index SDL last told the device
    bool can_rumble = true;  // false: it has no rumble, as SDL sees it
    bool refuses = false;    // a rumble asked of it fails, saying why
    const char* why = "virtual pad refuses rumble";
    Uint16 vendor = 0, product = 0;  // its USB IDs, from which SDL takes its type and labels
    Uint32 button_mask = 0;  // a gamepad's buttons, a bit for each SDL_GamepadButton; 0 is all
};

// SDL started for the gamepads with no joystick connected. False, having said why, if not.
bool sdl_up();
// The player index SDL has for `v`.
int player(const Virtual& v);
// Plugs `v` in: a gamepad with every axis SDL has and every button, or those in its
// button_mask, in SDL's order, or a plain joystick with two axes and four buttons, which SDL
// has no mapping for. With `index`, its player index is set to that before the frontend hears
// of it, as Steam Input's slot or an XInput user index would be.
void attach(Virtual& v, int index = kSdlIndex);
void detach(Virtual& v);
std::string guid_of(const Virtual& v);

// One frame of the desktop frontend: the window's events, then all four ports.
void frame();
// Plugs each of `vs` in, in turn, with a frame after each, so each lands before the next comes.
void plug(std::initializer_list<Virtual*> vs);
// Pulls each of `vs` out, then a frame.
void unplug(std::initializer_list<Virtual*> vs);
// Port `i` (0 is port 1) as the guest reads it at presented frame `at`.
PadState read(int i, uint32_t at = 0);
bool at_rest(const PadState& p);
// A port as one line: connected, its buttons, sticks and triggers.
std::string describe(const PadState& p);
// `v`'s `button` pressed or let go, then a frame.
void press(const Virtual& v, int button, bool down);
// The port `v` drives, 1 to 4, found by pressing its A and reading all four ports. 0 for
// none; -1 if more than one port saw it, or if the player index SDL has for the device,
// or the one it last told the device, is not that port's.
int port(const Virtual& v);
// The game's poll command for port `i`, with the rumble motor on or off.
void set_motor(int i, bool on);
// The game's poll command for port `i` with the rumble motor on, for one frame.
void motor(int i);
// What main() does with --pads=N, and what the guest's power-on does with it (si_init):
// a copy of main()'s lines, not main() itself.
void power_on(int pads);
// A controls.txt at `path` that says `text`, loaded.
void load_controls(const std::string& path, const char* text);
// The controls.txt in `dir` that isn't there loaded, which binds nothing.
void no_controls(const std::string& dir);

bool write_file(const std::string& path, const std::string& text);
std::string read_file(const std::string& path);
// The file's identity. A write renames a new file over the old one, so its inode changes
// with every write and with nothing else. 0 for no file.
uint64_t file_id(const std::string& path);
bool exists_path(const std::string& path);
bool has(const std::string& text, const std::string& what);
// How many times the file at `path` has `what`.
int lines_with(const std::string& path, const char* what);
// The lines of the file at `path` that start with `prefix`.
std::string lines_starting(const std::string& path, const char* prefix);
// The line ports.txt has for `v` on `port` (1 to 4). The virtual pads have no serial and no
// USB IDs.
std::string line_for(int port, const Virtual& v);
// A launch from nothing. The file is gone, and it's read again at the first gamepad.
void fresh_file(const std::string& file);
// An input log written by hand: `text` as the inputs.txt of the folder `log`, made for it.
void write_log(const std::string& log, const char* text);

// From here stderr goes to `path`. The stderr it had, for stderr_back.
int stderr_to(const std::string& path);
// stderr back to `saved`, then what went to `path` printed.
void stderr_back(int saved, const std::string& path);
// Runs `f` with stderr going to `path`, then prints what it wrote.
void capture(const std::string& path, const std::function<void()>& f);

#ifdef GCN_OVERLAY
// A thread stands in for the guest. Each host frame lets it reach one more frame boundary
// (pause_gate), after the frontend has taken its events and set the ports.
struct Guest {
    std::mutex m;
    std::condition_variable cv;
    uint32_t asked = 0;               // the boundaries it may reach
    std::atomic<uint32_t> passed{0};  // the boundaries it's gone past
    bool quit = false;
    std::thread t{[this] { run(); }};
    void run() {
        for (uint32_t n = 0;; n++) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return quit || asked > n; });
                if (quit) return;
            }
            pause_gate(n);
            passed = n + 1;
        }
    }
    ~Guest() {
        pause_request(false);
        {
            std::lock_guard<std::mutex> lk(m);
            quit = true;
        }
        cv.notify_all();
        t.join();
    }
};

// One host frame, the menu's included where there's a window, then the guest on past its next
// boundary, or held there. pump_events' answer.
bool host_frame(Guest& g);
// SDL's gamepad button `button` on `v`, which a virtual gamepad numbers among the buttons it
// has, in SDL's order.
void set_button(const Virtual& v, int button, bool down);
// A gamepad's button pressed and let go, a host frame each.
void tap(Guest& g, const Virtual& v, int button);

extern Settings g_applied;  // what the Display page last had the frontend apply
extern int g_applies;
void record_apply(const Settings& s);
extern int g_page_draws;  // the added page's draws
void draw_added_page(void* user);
// The menu's pages back to its own, then "Extra" added after them, its draws counted from 0.
void add_extra_page();

// A window from SDL's dummy video driver, which the menu takes keys from, checked. Null if none.
SDL_Window* open_window();
void close_window(SDL_Window* window);
// A key pressed, or let go, over the window, or a held one's repeat, then a host frame. False
// if pump_events was.
bool key(Guest& g, SDL_Scancode scancode, SDL_Keycode keycode, bool down, bool repeat = false);
// A key pressed and let go over the window, a host frame each. False if pump_events was.
bool tap(Guest& g, SDL_Scancode scancode, SDL_Keycode keycode);
// From the top of the page down past every row and left past every column, on `v`'s D-pad or
// on the keyboard's arrows: the bottom row's first button.
void bottom_row(Guest& g, const Virtual& v);
void bottom_row(Guest& g);
// The menu open over the held game, asking `asking`, and no restart on its way.
bool waiting(OverlayAsk asking);

// Where the menu's tab titled `title` is: its tab bar and its place there. Null and -1 for none.
struct TabAt {
    ImGuiTabBar* bar = nullptr;
    int index = -1;
};
TabAt find_tab(const char* title);
// The menu's tab titled `title` picked, as a click on it would, then drawn. False if there's none.
bool pick_tab(Guest& g, const char* title);
#endif

}  // namespace
