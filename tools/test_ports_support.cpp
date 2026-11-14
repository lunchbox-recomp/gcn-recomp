// What tools/test_ports_support.h declares. tools/test_ports.cpp reads it in after the frontend,
// whose input loop (pump_events, update_pads) and SI layer (pad_get, g_outbuf) it drives.
#include "test_ports_support.h"
#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_checks, g_failed;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failed++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
}

void say_passed(const char* what) {
    printf("test_ports %s: %d of %d checks passed\n", what, g_checks - g_failed, g_checks);
}

int finish(const char* what) {
    SDL_Quit();
    say_passed(what);
    return g_failed ? 1 : 0;
}

void finish_now(const char* what) {
    say_passed(what);
    fflush(stdout);
    _exit(g_failed ? 1 : 0);
}

bool SDLCALL on_rumble(void* u, Uint16 low, Uint16 high) {
    Virtual* v = static_cast<Virtual*>(u);
    if (low || high) v->rumbles++;
    else v->stops++;
    return v->refuses ? SDL_SetError("%s", v->why) : true;
}

void SDLCALL on_player(void* u, int index) { static_cast<Virtual*>(u)->player = index; }

bool sdl_up() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    printf("SDL %d.%d.%d (%s), platform \"%s\"\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
           SDL_GetRevision(), SDL_GetPlatform());
    for (int i = 0; i < 20; i++) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    int found = 0;
    SDL_free(SDL_GetJoysticks(&found));
    if (found > 0) {
        printf("needs the ports to itself: disconnect the %d joystick(s) first\n", found);
        SDL_Quit();
        return false;
    }
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    return true;
}

int player(const Virtual& v) { return SDL_GetJoystickPlayerIndexForID(v.id); }

void attach(Virtual& v, int index) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = v.gamepad ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
    d.naxes = v.gamepad ? SDL_GAMEPAD_AXIS_COUNT : 2;
    d.nbuttons = !v.gamepad ? 4 : v.button_mask ? __builtin_popcount(v.button_mask) : SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask = v.button_mask;
    d.name = v.name;
    d.vendor_id = v.vendor;
    d.product_id = v.product;
    d.userdata = &v;
    d.Rumble = v.can_rumble ? on_rumble : nullptr;
    d.SetPlayerIndex = on_player;
    v.id = SDL_AttachVirtualJoystick(&d);
    if (!v.id) fprintf(stderr, "SDL_AttachVirtualJoystick: %s\n", SDL_GetError());
    if (index != kSdlIndex && v.id) {
        SDL_Joystick* j = SDL_OpenJoystick(v.id);
        SDL_SetJoystickPlayerIndex(j, index);
        SDL_CloseJoystick(j);
    }
}

void detach(Virtual& v) {
    if (!v.id) return;
    if (!SDL_DetachVirtualJoystick(v.id)) fprintf(stderr, "SDL_DetachVirtualJoystick: %s\n", SDL_GetError());
    v.id = 0;
}

std::string guid_of(const Virtual& v) {
    char guid[33];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(v.id), guid, sizeof(guid));
    return guid;
}

void frame() {
    pump_events();
    update_pads(0);
}

void plug(std::initializer_list<Virtual*> vs) {
    for (Virtual* v : vs) {
        attach(*v);
        frame();
    }
}

void unplug(std::initializer_list<Virtual*> vs) {
    for (Virtual* v : vs) detach(*v);
    frame();
}

PadState read(int i, uint32_t at) {
    gx::g_frames_submitted = at;
    const PadState p = pad_get(i);
    gx::g_frames_submitted = 0;
    return p;
}

bool at_rest(const PadState& p) {
    return !p.buttons && p.stick_x == 0x80 && p.stick_y == 0x80 && p.cstick_x == 0x80 && p.cstick_y == 0x80 &&
           !p.trig_l && !p.trig_r;
}

std::string describe(const PadState& p) {
    char s[64];
    snprintf(s, sizeof(s), "%d %04X %02X %02X %02X %02X %02X %02X\n", p.connected ? 1 : 0, p.buttons, p.stick_x, p.stick_y,
             p.cstick_x, p.cstick_y, p.trig_l, p.trig_r);
    return s;
}

void press(const Virtual& v, int button, bool down) {
    if (SDL_Joystick* j = v.id ? SDL_GetJoystickFromID(v.id) : nullptr) SDL_SetJoystickVirtualButton(j, button, down);
    frame();
}

int port(const Virtual& v) {
    if (!v.id || !SDL_GetJoystickFromID(v.id)) return 0;
    press(v, SDL_GAMEPAD_BUTTON_SOUTH, true);
    int found = 0, n = 0;
    for (int i = 0; i < 4; i++)
        if (read(i).buttons & PAD_A) { found = i + 1; n++; }
    press(v, SDL_GAMEPAD_BUTTON_SOUTH, false);
    if (n > 1) return -1;
    if (!found) return 0;
    return player(v) == found - 1 && v.player == found - 1 ? found : -1;
}

void set_motor(int i, bool on) { g_outbuf[i] = on ? 0x00400001 : 0x00400000; }

void motor(int i) {
    set_motor(i, true);
    frame();
    set_motor(i, false);
    frame();
}

void power_on(int pads) {
    pad_set_count(pads);
    g_port_count = std::max(pads, input_map_port_count());
    pad_set_count(g_port_count);
    si_init();
}

void load_controls(const std::string& path, const char* text) {
    write_file(path, text);
    input_map_load(path.c_str());
}

void no_controls(const std::string& dir) { input_map_load((dir + "/no-controls.txt").c_str()); }

bool write_file(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fputs(text.c_str(), f);
    return fclose(f) == 0;
}

std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = fopen(path.c_str(), "r")) {
        char line[512];
        while (fgets(line, sizeof(line), f)) out += line;
        fclose(f);
    }
    return out;
}

uint64_t file_id(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_ino : 0;
}

bool exists_path(const std::string& path) { return file_id(path) != 0; }

bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

int lines_with(const std::string& path, const char* what) {
    int n = 0;
    const std::string text = read_file(path);
    for (size_t at = 0; (at = text.find(what, at)) != std::string::npos; at++) n++;
    return n;
}

std::string lines_starting(const std::string& path, const char* prefix) {
    std::string out;
    const std::string text = read_file(path);
    for (size_t at = 0; at < text.size();) {
        const size_t next = std::min(text.find('\n', at), text.size() - 1) + 1;
        if (text.compare(at, strlen(prefix), prefix) == 0) out += text.substr(at, next - at);
        at = next;
    }
    return out;
}

std::string line_for(int port, const Virtual& v) { return "\n" + std::to_string(port) + " " + guid_of(v) + " - 0000:0000\n"; }

void fresh_file(const std::string& file) {
    remove(file.c_str());
    gamepads_remember(file.c_str());
}

void write_log(const std::string& log, const char* text) {
    plat_make_dirs(log);
    write_file(log + "/inputs.txt", text);
}

int stderr_to(const std::string& path) {
    fflush(stderr);
    const int saved = dup(2);
    if (FILE* f = fopen(path.c_str(), "w")) {
        dup2(fileno(f), 2);
        fclose(f);
    }
    return saved;
}

void stderr_back(int saved, const std::string& path) {
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    printf("---- stderr\n%s----\n", read_file(path).c_str());
}

void capture(const std::string& path, const std::function<void()>& f) {
    const int saved = stderr_to(path);
    f();
    stderr_back(saved, path);
}

#ifdef GCN_OVERLAY
bool host_frame(Guest& g) {
    const bool running = pump_events();
    update_pads(0);
    overlay_menu_frame();
    guest_on(g);
    return running;
}

void guest_on(Guest& g) {
    uint32_t want;
    {
        std::lock_guard<std::mutex> lk(g.m);
        want = ++g.asked;
    }
    g.cv.notify_all();
    for (int i = 0; i < 2000 && g.passed < want && !(pause_requested() && paused()); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void set_button(const Virtual& v, int button, bool down) {
    const int index = v.button_mask ? __builtin_popcount(v.button_mask & ((1u << button) - 1)) : button;
    if (SDL_Joystick* j = SDL_GetJoystickFromID(v.id)) SDL_SetJoystickVirtualButton(j, index, down);
}

void tap(Guest& g, const Virtual& v, int button) {
    set_button(v, button, true);
    host_frame(g);
    set_button(v, button, false);
    host_frame(g);
}

Settings g_applied;
int g_applies;
void record_apply(const Settings& s) {
    g_applied = s;
    g_applies++;
}

int g_page_draws;
void draw_added_page(void* user) { ++*static_cast<int*>(user); }

void add_extra_page() {
    overlay_test_reset_pages();
    g_page_draws = 0;
    overlay_add_page("Extra", draw_added_page, &g_page_draws);
}

SDL_WindowID g_window_id;  // the test window's, which the menu takes keys from

SDL_Window* open_window() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_Window* window = SDL_InitSubSystem(SDL_INIT_VIDEO) ? SDL_CreateWindow("test_ports", 1280, 960, SDL_WINDOW_HIDDEN) : nullptr;
    check(window != nullptr, "SDL's dummy video driver makes a window for the menu");
    if (window) g_window_id = SDL_GetWindowID(window);
    return window;
}

void close_window(SDL_Window* window) {
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void push_key(SDL_Scancode scancode, SDL_Keycode keycode, bool down, bool repeat) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = g_window_id;
    e.key.scancode = scancode;
    e.key.key = keycode;
    e.key.down = down;
    e.key.repeat = repeat;
    SDL_PushEvent(&e);
}

bool key(Guest& g, SDL_Scancode scancode, SDL_Keycode keycode, bool down, bool repeat) {
    push_key(scancode, keycode, down, repeat);
    return host_frame(g);
}

bool tap(Guest& g, SDL_Scancode scancode, SDL_Keycode keycode) {
    const bool pressed = key(g, scancode, keycode, true);
    return key(g, scancode, keycode, false) && pressed;
}

void bottom_row(Guest& g, const Virtual& v) {
    for (int i = 0; i < 8; i++) tap(g, v, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    for (int i = 0; i < 2; i++) tap(g, v, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
}

void bottom_row(Guest& g) {
    for (int i = 0; i < 8; i++) tap(g, SDL_SCANCODE_DOWN, SDLK_DOWN);
    for (int i = 0; i < 2; i++) tap(g, SDL_SCANCODE_LEFT, SDLK_LEFT);
}

bool waiting(OverlayAsk asking) { return overlay_open() && paused() && overlay_asking() == asking && !g_restart; }

int page_at(const char* title) {
    const std::vector<std::string>& pages = overlay_test_pages();
    const auto at = std::find(pages.begin(), pages.end(), title);
    return at == pages.end() ? -1 : (int)(at - pages.begin());
}

bool pick_page(Guest& g, const char* title) {
    const int page = page_at(title);
    if (page < 0) return false;
    overlay_test_pick_page(page);
    host_frame(g);
    host_frame(g);
    return true;
}
#endif

}  // namespace
