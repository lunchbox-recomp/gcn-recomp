// Entry point and SDL frontend: window, input, audio. The main thread executes GX
// batches produced by the guest thread.
#include "runtime.h"
#include "platform.h"
#include "input_script.h"
#include "input_log.h"
#include "input_map.h"
#include "gamepads.h"
#include "gx/render_gl.h"
#include "gx/aspect.h"
#include "gx/gl.h"
#include "hw/pad.h"
#include "vr_game.h"
#include "telemetry.h"
#include "record.h"
#include "settings.h"
#include "session.h"
#include "pause.h"
#ifdef GCN_OPENXR
#include "xr_desktop.h"
#endif
// The engine's commit, which the build writes at every build (tools/engine_sha.cmake).
#if __has_include("gcn_engine_sha.h")
#include "gcn_engine_sha.h"
#else
#define GCN_ENGINE_SHA ""
#endif
// SDL_main.h is left out: this is a console program with its own main(), on Windows too.
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

uint32_t boot_load(const char* iso_path);
void debug_dump_threads();
void debug_sampler_start();
bool write_png(const char* path, const uint8_t* rgba, int w, int h);

// ---------------------------------------------------------------------------
// --eye renders through the stereo path into an offscreen target and shows that in the
// window instead of the flat frame: the eye sits where the game's camera is, looking
// straight ahead with a 90 degree field, so a VR change can be looked at here before it
// goes anywhere near a headset. --first-person puts the eye wherever the game project's
// eye hook says (render_set_first_person), on the player's vehicle for a racing game.
// --dump-dir/--dump-every write the eye's frames as PNGs.
// ---------------------------------------------------------------------------
static bool g_eye_mode = false;
static GLuint g_eye_fbo, g_eye_tex, g_eye_depth;
static const int kEyeW = 960, kEyeH = 720;
// The game's VR settings (eye_init), for the eye's planes and the HUD frame's placement.
static VrConfig g_eye_cfg;

static void eye_init() {
    glGenTextures(1, &g_eye_tex);
    glBindTexture(GL_TEXTURE_2D, g_eye_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEyeW, kEyeH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &g_eye_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_eye_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kEyeW, kEyeH);
    glGenFramebuffers(1, &g_eye_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_eye_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_eye_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_eye_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("eye fbo incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The game's VR settings as a headset would have them -- its defaults, then ./vr.txt --
    // so the preview shows what the game's hooks do: its world pitch, its depth layers and
    // what its eye filter leaves out.
    const VrConfig& c = g_eye_cfg = vr::load_config(".");
    gx::render_set_world_pitch(c.world_pitch_deg * 3.14159265f / 180.0f);
    gx::render_set_depth_layers(c.background_band, c.foreground_band, c.foreground_scale, c.hud_band, c.hud_band_scale);
    gx::render_set_hud_push(c.hud_push_below, c.hud_push_to);
    gx::render_set_view_space_world(c.view_space_world);
    gx::render_set_eye_filter(vr::game_hooks().eye_filter);
    gx::render_set_vr_morph(1.0f, nullptr);
}

// Column-major, as glUniformMatrix4fv takes them. The view is the identity: the eye is
// wherever the renderer's world transform puts it.
static void eye_matrices(float* proj, float* view, float* hud) {
    const float fov = 1.0f;   // tan(45 deg): a 90 degree vertical field
    const float aspect = (float)kEyeW / (float)kEyeH;
    const float u = g_eye_cfg.units_per_metre;
    const float n = g_eye_cfg.near_m * u, f = g_eye_cfg.far_m * u;
    memset(proj, 0, 16 * sizeof(float));
    proj[0] = 1.0f / (fov * aspect);
    proj[5] = 1.0f / fov;
    proj[10] = -(f + n) / (f - n);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * f * n) / (f - n);
    memset(view, 0, 16 * sizeof(float));
    view[0] = view[5] = view[10] = view[15] = 1.0f;
    gx::render_hud_frame(g_eye_cfg.hud_distance_m * u, fov, g_eye_cfg.hud_scale, g_eye_cfg.hud_height_m * u,
                         g_eye_cfg.hud_pitch_deg * 3.14159265f / 180.0f, hud);
}

static void eye_dump(uint32_t n) {
    std::vector<uint8_t> px((size_t)kEyeW * kEyeH * 4), fl(px.size());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_eye_fbo);
    glReadPixels(0, 0, kEyeW, kEyeH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const size_t stride = (size_t)kEyeW * 4;
    for (int y = 0; y < kEyeH; y++) memcpy(&fl[y * stride], &px[(size_t)(kEyeH - 1 - y) * stride], stride);
    for (size_t i = 3; i < fl.size(); i += 4) fl[i] = 255;
    char path[512];
    snprintf(path, sizeof(path), "%s/eye_%05u.png", gx::g_dump_dir, n);
    write_png(path, fl.data(), kEyeW, kEyeH);
}

// The eye's newest frame, stretched to the window's dw by dh pixels. False, with nothing
// drawn, before the eye has drawn one.
static bool eye_to_window(int dw, int dh) {
    if (!gx::present_count()) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_eye_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, kEyeW, kEyeH, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    return true;
}

// Stringify, to quote the required GL version in a message without a format argument.
#define GCN_STR_(x) #x
#define GCN_STR(x) GCN_STR_(x)

// SDL3 hands GL entry points back as function pointers; the loader takes them as void*.
static void* gl_proc(const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }

// The gamepads and a wheel's force feedback are the main loop's, but an interrupt stops
// their rumble and force from the interrupt thread (plat_install_crash_handlers). Every
// use of them holds this.
static std::mutex g_pads_mutex;

static void on_interrupt() {
    // GCN_RAM_DUMP=<path>: the guest RAM at the interrupt, for reading the game's state offline.
    if (const char* path = getenv("GCN_RAM_DUMP")) {
        FILE* f = fopen(path, "wb");
        if (!f) perror("GCN_RAM_DUMP");
        else { fwrite(g_mem, 1, RAM_SIZE, f); fclose(f); fprintf(stderr, "RAM written to %s\n", path); }
    }
    plat_watchdog(2, 2);  // never hang in here (stdio locks may be held by other threads)
    input_log_end();
    {
        std::lock_guard<std::mutex> lk(g_pads_mutex);
        input_map_shutdown();  // a wheel left pushing is a wheel pushing at nobody
        gamepads_shutdown();   // and a gamepad left rumbling, at nobody
    }
    debug_dump_threads();
    plat_exit_now(1);
}

static void on_fault(const void* fault_addr, int code) {
    if (const char* path = getenv("GCN_RAM_DUMP")) {
        if (FILE* f = fopen(path, "wb")) { fwrite(g_mem, 1, RAM_SIZE, f); fclose(f); fprintf(stderr, "RAM written to %s\n", path); }
    }
    uintptr_t a = (uintptr_t)fault_addr;
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x40000000)
        fprintf(stderr, "\nFAULT: guest memory access at (addr & 0x3FFFFFFF) = %08lX\n", (unsigned long)(a - (uintptr_t)g_mem));
    else
        fprintf(stderr, "\nFAULT: host address %p (code %d)\n", fault_addr, code);
    plat_watchdog(2, 2);
    input_log_end();
    input_map_shutdown();
    gamepads_shutdown();
    plat_backtrace_print();
    debug_dump_threads();
    plat_exit_now(1);
}

bool audio_open();

// The refresh rate of the display the window is on, for a session's record, or 0.
static double display_hz(SDL_Window* win) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
    return mode ? mode->refresh_rate : 0;
}

// ---------------------------------------------------------------------------
// Input: the keyboard on port 1, each gamepad on the port it is given (gamepads.h), and
// controls.txt on the ports it names (input_map.h). --pads=N plugs in ports 1 to N
// whether or not anything drives them, at rest unless a script presses them (the SI layer
// applies scripts, input_script.h).
// ---------------------------------------------------------------------------
static int g_port_count = 1;  // plugged in from power-on: --pads, or the ports controls.txt binds

static void merge_keyboard(PadState& p) {
    const bool* k = SDL_GetKeyboardState(nullptr);
    if (k[SDL_SCANCODE_RETURN]) p.buttons |= PAD_START;
    if (k[SDL_SCANCODE_X]) p.buttons |= PAD_A;
    if (k[SDL_SCANCODE_Z]) p.buttons |= PAD_B;
    if (k[SDL_SCANCODE_C]) p.buttons |= PAD_X;
    if (k[SDL_SCANCODE_S]) p.buttons |= PAD_Y;
    if (k[SDL_SCANCODE_D]) p.buttons |= PAD_Z;
    if (k[SDL_SCANCODE_Q]) { p.buttons |= PAD_L; p.trig_l = 255; }
    if (k[SDL_SCANCODE_W]) { p.buttons |= PAD_R; p.trig_r = 255; }
    if (k[SDL_SCANCODE_I]) p.buttons |= PAD_UP;
    if (k[SDL_SCANCODE_K]) p.buttons |= PAD_DOWN;
    if (k[SDL_SCANCODE_J]) p.buttons |= PAD_LEFT;
    if (k[SDL_SCANCODE_L]) p.buttons |= PAD_RIGHT;
    int sx = 0, sy = 0;
    if (k[SDL_SCANCODE_LEFT]) sx -= 100;
    if (k[SDL_SCANCODE_RIGHT]) sx += 100;
    if (k[SDL_SCANCODE_UP]) sy += 100;
    if (k[SDL_SCANCODE_DOWN]) sy -= 100;
    if (sx || sy) {
        p.stick_x = (uint8_t)(128 + sx);
        p.stick_y = (uint8_t)(128 + sy);
    }
}

// What drives `port`, merged into `p`: buttons are added, and a stick or trigger is taken
// over only while it is pushed, so that port 1 can be shared with a headset's controllers
// (xr_desktop.cpp). Then the game's rumble motor for the port goes to its gamepad, and to a
// wheel's force feedback, except while the guest is held: a pause stops the motor with it.
static bool g_guest_held;  // the loop's, from paused() (pause.h)
static void merge_port(int port, PadState& p) {
    const bool motor = pad_motor(port) && !g_guest_held;
    std::lock_guard<std::mutex> lk(g_pads_mutex);
    if (port == 0) merge_keyboard(p);
    gamepads_merge(port, p);
    input_map_apply(p, port, motor);
    gamepads_rumble(port, motor);
}

static void merge_desktop_pad(PadState& p) { merge_port(0, p); }

// Ports `first` to 4. A port is plugged in while it has a gamepad, and from power-on if
// --pads or controls.txt reaches it; port 1, the keyboard's, always is.
static void update_pads(int first) {
    for (int port = first; port < 4; port++) {
        PadState p;
        p.connected = port < g_port_count;
        merge_port(port, p);
        pad_set_state(port, p);
    }
}

// The window's events. False once the user has asked to quit.
static bool pump_events() {
    SDL_Event e;
    bool running = true;
    while (SDL_PollEvent(&e)) {
        {
            std::lock_guard<std::mutex> lk(g_pads_mutex);
            input_map_event(e);
            gamepads_event(e);
        }
        if (e.type == SDL_EVENT_QUIT) running = false;
        else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
        // F11 switches fullscreen. On macOS Control-Command-F does too, through the Window menu
        // SDL gives the app, so it isn't handled here as well: that would switch twice.
        else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
            if (SDL_Window* w = SDL_GetWindowFromID(e.key.windowID))
                SDL_SetWindowFullscreen(w, !(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN));
        }
    }
    return running;
}

// What has been drawn to the window goes to it, and to the movie if one is being made.
static void show_frame(SDL_Window* win) {
    // macOS (GL on Metal) only presents correctly with the window framebuffer bound.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (record::active()) {
        int rw, rh;
        SDL_GetWindowSizeInPixels(win, &rw, &rh);
        record::frame(0, rw, rh);
    }
    SDL_GL_SwapWindow(win);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    // GCN_SESSION sets the switches a session turns on, so it goes before anything reads one.
    const std::string session_inputs = session_begin(argc, argv);
    // The game resetting the console starts it again in a fresh process, as a power cycle
    // would -- except in a replay, which would only replay itself from the start again.
    static char** s_argv = argv;
    set_reset_handler([](uint32_t, uint32_t) {
        if (input_replay_active()) return;
        session_relaunching();
        plat_relaunch(s_argv);
    });
    // Default: the image the build was configured with, else the first image in ./rom
    std::string iso_default = GCN_DEFAULT_ISO;
    if (!plat_readable(iso_default.c_str())) iso_default = plat_find_file("rom", ".iso");
    if (iso_default.empty()) iso_default = plat_find_file("rom", ".ciso");
    const char* iso = iso_default.c_str();
    bool headless = false, hidden = false, input_log = true, first_person = false, fast = false, vr = false;
    uint32_t exit_after = 0;  // --exit-after=N: quit once N frames have been presented
    std::string input_log_dir, replay_dir;
    // --record=<file>[ --record-size=WxH]: a movie of the window with the game's audio (record.h).
    const char* record_to = nullptr;
    int record_w = 2560, record_h = 1440;
    const char* controls = "controls.txt";
    bool test_ffb = false;
    // --telemetry[=host:port]: the game's motion to a motion platform (telemetry.h).
    const char* telemetry_to = getenv("GCN_TELEMETRY");
    // settings.txt in the working directory (settings.h). A flag beats it for this run and
    // isn't written back.
    const Settings settings = settings_load("settings.txt");
    int scale = settings.scale;
    int pads = 1;
    // --aspect=W:H, or --aspect=window to follow the window's shape: what the picture is
    // stretched to (gx::output_aspect). 4:3, the console's own, by default.
    int aspect_w = 4, aspect_h = 3;
    bool aspect_window = false, fullscreen = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log-all")) for (auto& e : g_log_enabled) e = true;
        else if (!strncmp(argv[i], "--log=", 6)) {
            if (!log_enable_list(argv[i] + 6)) { fprintf(stderr, "--log: unknown category in %s (cpu,os,hw,dvd,gx,vi,si,exi,dsp,ai,thr)\n", argv[i] + 6); return 1; }
        }
        else if (!strcmp(argv[i], "--eye")) g_eye_mode = true;
        else if (!strcmp(argv[i], "--vr")) vr = true;
        else if (!strcmp(argv[i], "--list-controls")) return input_map_probe();
        else if (!strcmp(argv[i], "--test-ffb")) test_ffb = true;
        else if (!strncmp(argv[i], "--controls=", 11)) controls = argv[i] + 11;
        else if (!strncmp(argv[i], "--pads=", 7)) {
            pads = atoi(argv[i] + 7);
            if (pads < 1 || pads > 4) { fprintf(stderr, "--pads: 1 to 4 controllers, not %s\n", argv[i] + 7); return 1; }
        }
        else if (!strcmp(argv[i], "--telemetry")) telemetry_to = "";
        else if (!strncmp(argv[i], "--telemetry=", 12)) telemetry_to = argv[i] + 12;
        else if (!strcmp(argv[i], "--first-person")) first_person = true;
        else if (!strcmp(argv[i], "--no-input-log")) input_log = false;
        else if (!strncmp(argv[i], "--input-log=", 12)) input_log_dir = argv[i] + 12;
        else if (!strncmp(argv[i], "--replay=", 9)) replay_dir = argv[i] + 9;
        else if (!strcmp(argv[i], "--sample")) debug_sampler_start();
        else if (!strcmp(argv[i], "--no-music")) { void audio_mute_music(bool); audio_mute_music(true); }
        else if (!strncmp(argv[i], "--record=", 9)) record_to = argv[i] + 9;
        else if (!strncmp(argv[i], "--record-size=", 14)) sscanf(argv[i] + 14, "%dx%d", &record_w, &record_h);
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--hidden")) hidden = true;
        else if (!strcmp(argv[i], "--fast")) fast = true;
        else if (!strncmp(argv[i], "--exit-after=", 13)) exit_after = (uint32_t)strtoul(argv[i] + 13, nullptr, 10);
        else if (!strcmp(argv[i], "--cull-swap")) gx::g_cull_swap = true;
        else if (!strncmp(argv[i], "--scale=", 8)) scale = atoi(argv[i] + 8);
        else if (!strcmp(argv[i], "--aspect=window")) aspect_window = true;
        else if (!strncmp(argv[i], "--aspect=", 9)) {
            if (!gx::parse_aspect(argv[i] + 9, &aspect_w, &aspect_h)) {
                fprintf(stderr, "--aspect: W:H, two whole numbers from 1 to 9999, or window, not %s\n", argv[i] + 9);
                return 1;
            }
            aspect_window = false;
        }
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = true;
        else if (!strncmp(argv[i], "--dump-dir=", 11)) gx::g_dump_dir = argv[i] + 11;
        else if (!strncmp(argv[i], "--dump-every=", 13)) gx::g_dump_every = atoi(argv[i] + 13);
        else if (!strcmp(argv[i], "--dump-output")) gx::g_dump_output = true;
        else if (argv[i][0] != '-') iso = argv[i];
    }
    // --eye and GCN_THEATER_STEREO (below) show a headset's views on the desktop. A headset's
    // panel keeps the console's 4:3 whatever the output aspect is, so these ignore it: the
    // window opens at 4:3 and the game's hooks see 4:3, not a shape the view isn't drawn at.
    const float theater_stereo_m = getenv("GCN_THEATER_STEREO") ? (float)atof(getenv("GCN_THEATER_STEREO")) : 0.0f;
    if ((g_eye_mode || theater_stereo_m > 0.0f) && (aspect_window || aspect_w * 3 != aspect_h * 4)) {
        fprintf(stderr, "--aspect is ignored with --eye and GCN_THEATER_STEREO, whose views stay 4:3\n");
        aspect_w = 4;
        aspect_h = 3;
        aspect_window = false;
    }
    if (test_ffb) return input_map_test_ffb(controls);
    plat_install_crash_handlers(on_interrupt, on_fault);
    if (record_to && !record::start(record_to, record_w, record_h, 60)) return 1;
    if (telemetry_to && !telemetry::configure(telemetry_to)) {
        fprintf(stderr, "--telemetry: could not set up the telemetry socket\n");
        return 1;
    }
#ifndef GCN_OPENXR
    if (vr) {
        fprintf(stderr, "--vr: this build has no OpenXR frontend (GCN_OPENXR is off, or this platform has none).\n");
        return 1;
    }
#endif

    if (!plat_readable(iso)) {
        fprintf(stderr, "No game image found. Put your " GCN_GAME_TITLE " .iso or .ciso in rom/ or pass its path.\n");
        return 1;
    }
    mem_init();
    timing_init();
    if (fast) clock_set_scale(0);  // run as fast as the host allows; the game sees 60 Hz regardless
    pad_set_count(pads);
    input_script_init();

    // Every run records what the guest read from the controller, so that a route to
    // wherever something went wrong can be played back with --replay=<dir>. See
    // input_log.h. --no-input-log turns it off; --input-log=<dir> names the directory, and
    // without it a session (GCN_SESSION) puts the log in its own.
    // The memory card and the SRAM are snapshotted into the log, and a replay runs on
    // scratch copies of those snapshots rather than the real ones.
    std::string replay_card, replay_sram;
    if (!replay_dir.empty()) {
        if (!input_replay_load(replay_dir, replay_card, replay_sram)) return 1;
        if (!replay_card.empty()) g_memcard_path = replay_card.c_str();
        g_sram_path = replay_sram.c_str();
    }
    if (input_log) {
        if (input_log_dir.empty()) input_log_dir = session_inputs;
        if (input_log_dir.empty()) {
            char stamp[64];
            time_t now = time(nullptr);
            strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
            input_log_dir = std::string("saves/inputs/") + stamp;
        }
        input_log_start(input_log_dir, replay_card.empty() ? "saves/memcard_a.raw" : replay_card,
                        replay_sram.empty() ? "saves/sram.raw" : replay_sram);
    }

    if (headless) {
        // No window and no sound device, so session.json has no display or device to name. It
        // is written all the same: a reset relaunches into the next run-N by finding it.
        session_write(0, GCN_ENGINE_SHA);
        uint32_t entry = boot_load(iso);
        threads_start_boot(entry);
        for (;;) {
            auto b = gx::take_batch(5);  // discard
            (void)b;
        }
    }

    // Joysticks are read without the window's focus: in --vr the headset has it, and a
    // wheel or pedals have no reason to stop when another window is clicked.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC)) fatal("SDL_Init: %s", SDL_GetError());
    {
        std::lock_guard<std::mutex> lk(g_pads_mutex);
        input_map_load(controls);
        gamepads_load_mappings();
        gamepads_init();
    }
    // Ports 1 to the highest controls.txt binds are plugged in at power-on, as --pads would.
    g_port_count = std::max(pads, input_map_port_count());
    pad_set_count(g_port_count);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    const bool gl_debug = getenv("GCN_GL_DEBUG") != nullptr;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG |
                                                  (gl_debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0));
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // 960 high and as wide as the output aspect makes that, 1280 for 4:3 and for
    // --aspect=window, but never larger than the primary display's usable area: a wide aspect
    // is scaled down to fit, in its own shape. A window that isn't fullscreen opens hidden and
    // is then sized to the area its frame leaves and centred with the frame (aspect.h), so its
    // title bar can't land outside the area, before it's shown. The frame is SDL's where SDL
    // can say before the window is shown, as on Windows. Elsewhere SDL says nothing or 0, so a
    // title bar of kTitleBar points is allowed for, more than macOS's 28. The rect stays 0 by
    // 0 where SDL can't say what's usable, which is no limit, and SDL centres the window then.
    constexpr int kTitleBar = 40;
    SDL_Rect usable = {};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    const int shape_w = aspect_window ? 4 : aspect_w, shape_h = aspect_window ? 3 : aspect_h;
    const gx::WindowSize size = gx::aspect_window_size(960, shape_w, shape_h, usable.w, usable.h);
    const bool place = !fullscreen && usable.w > 0 && usable.h > 0;
    SDL_Window* win = SDL_CreateWindow(GCN_APP_LABEL, size.w, size.h,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                       (hidden || place ? SDL_WINDOW_HIDDEN : 0) | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) fatal("SDL_CreateWindow: %s", SDL_GetError());
    if (place) {
        int top = 0, left = 0, bottom = 0, right = 0;
        if (!SDL_GetWindowBordersSize(win, &top, &left, &bottom, &right) || !(top || left || bottom || right)) {
            top = kTitleBar;
            left = bottom = right = 0;
        }
        const gx::OutputRect r =
            gx::aspect_window_rect(960, shape_w, shape_h, {usable.x, usable.y, usable.w, usable.h}, top, left, bottom, right);
        SDL_SetWindowSize(win, r.w, r.h);
        SDL_SetWindowPosition(win, r.x, r.y);
        if (!hidden) SDL_ShowWindow(win);
    }
    // Ask for the newest core profile first: the renderer needs only GCN_GL_MAJOR.MINOR, but an
    // OpenXR runtime may want more of the context it shares (Meta's wants 4.0), and drivers
    // hand back exactly the version asked for.
    SDL_GLContext ctx = nullptr;
    static const int kTry[][2] = {{4, 6}, {4, 5}, {4, 3}, {4, 1}, {4, 0}};
    for (const auto& v : kTry) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, v[0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, v[1]);
        if ((ctx = SDL_GL_CreateContext(win))) break;
    }
    if (!ctx) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, GCN_GL_MAJOR);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, GCN_GL_MINOR);
        ctx = SDL_GL_CreateContext(win);
    }
    static const char* kGLHelp =
        "  The renderer needs an OpenGL " GCN_STR(GCN_GL_MAJOR) "." GCN_STR(GCN_GL_MINOR)
        " core profile. If this host only\n"
        "  offers legacy OpenGL (\"GDI Generic\" 1.1, typical for a VM with no GL driver),\n"
        "  install a GL implementation that provides it -- see docs/dev/graphics.md.";
    if (!ctx) fatal("SDL_GL_CreateContext: %s\n%s", SDL_GetError(), kGLHelp);
    SDL_GL_SetSwapInterval(0);
    // A legacy driver still "loads": it resolves the GL 1.1 exports and leaves every
    // 2.0+ entry point null, so check the version before handing off to the renderer.
    int glver = gl_load_with(gl_proc);
    if (glver < GCN_GL_VERSION_MIN) {
        fatal("OpenGL %d.%d is too old (got \"%s\" / \"%s\")\n%s",
              glver / 10, glver % 10, glGetString(GL_VERSION) ? (const char*)glGetString(GL_VERSION) : "?",
              glGetString(GL_RENDERER) ? (const char*)glGetString(GL_RENDERER) : "?", kGLHelp);
    }
    LOG(LOG_GX, "GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    if (gl_debug) gl_debug_enable(gl_proc);
    // Beside the memory card: both are state this machine accumulates for this game.
    plat_make_dirs("saves");
    gx::render_set_shader_cache("saves/shaders.bin");
#ifdef GCN_OPENXR
    // --vr: the headset takes over from here, the window staying up as a mirror.
    if (vr) {
        // Port 1 is the headset's controllers and merge_desktop_pad's; the others are
        // updated with the window's events.
        auto pump = [] {
            const bool running = pump_events();
            update_pads(1);
            return running;
        };
        // The headset frontend opens the sound device, so it is told to write session.json then.
        const int rc = xr_desktop_main({win, pump, merge_desktop_pad,
                                        [](SDL_Window* w) { session_write(display_hz(w), GCN_ENGINE_SHA); }},
                                       iso);
        record::finish();
        input_log_end();
        {
            std::lock_guard<std::mutex> lk(g_pads_mutex);
            input_map_shutdown();
            gamepads_shutdown();
        }
        threads_request_quit();
        plat_exit_now(rc);
    }
#endif
    gx::render_init(scale);
    // A paused guest's frame is drawn again in the loop below (render_present_last).
    gx::render_keep_last(true);
    // Before the guest starts, whose hooks may read it from the first frame. --vr has gone
    // its own way by here, so a headset's panel keeps 4:3, and --eye and GCN_THEATER_STEREO
    // are held to 4:3 above. --aspect=window takes the window's shape in the loop below.
    if (!aspect_window) gx::render_set_output_aspect((float)aspect_w / (float)aspect_h);
    if (g_eye_mode) eye_init();
    if (first_person) gx::render_set_first_person(true);
    if (gx::g_dump_dir) plat_make_dirs(gx::g_dump_dir);

    audio_open();
    session_write(display_hz(win), GCN_ENGINE_SHA);

    uint32_t entry = boot_load(iso);
    threads_start_boot(entry);

    bool running = true;
    uint32_t frames = 0;
    uint32_t presented_total = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (running) {
        running = pump_events();
        g_guest_held = paused();
        update_pads(0);
        int dw, dh;
        SDL_GetWindowSizeInPixels(win, &dw, &dh);
        gx::render_set_window_size(dw, dh);
        if (aspect_window && dh > 0) gx::render_set_output_aspect((float)dw / (float)dh);
        // While the guest is held (pause.h) no batch comes, so each 16 ms wait that ends with
        // nothing draws the newest frame again, and each one drawn is a host frame to the
        // pause. Before any frame there's nothing to draw, and no host frame.
        auto b = gx::take_batch(paused() ? 16 : 4);
        if (!b) {
            if (paused() && (g_eye_mode ? eye_to_window(dw, dh) : gx::render_present_last())) {
                show_frame(win);
                pause_host_frame();
            }
            continue;
        }
        // GCN_STEREOLOG=1: what the game's stereo hook (vr_game.h) would have a headset
        // show, whenever the answer changes, numbered as the frame dumps are.
        static const bool stereolog = getenv("GCN_STEREOLOG") != nullptr;
        if (stereolog && vr::game_hooks().wants_stereo) {
            static int last = -1;
            const int want = vr::game_hooks().wants_stereo(*b) ? 1 : 0;
            if (want != last) {
                last = want;
                fprintf(stderr, "[stereo] frame %u: %s\n", gx::present_count(), want ? "stereo" : "theater");
            }
        }
        bool presented;
        if (g_eye_mode) {
            float P[16], V[16], H[16];
            eye_matrices(P, V, H);
            gx::render_set_vr_eye(P, V, H);
            presented = gx::render_execute_eye(*b, g_eye_fbo, kEyeW, kEyeH, true);
            static uint32_t eye_frames = 0;
            eye_frames++;
            if (gx::g_dump_dir && gx::g_dump_every && eye_frames % gx::g_dump_every == 0) eye_dump(eye_frames);
            eye_to_window(dw, dh);
        } else if (theater_stereo_m > 0.0f &&
                   (!vr::game_hooks().wants_panel_pair || vr::game_hooks().wants_panel_pair(*b))) {
            // GCN_THEATER_STEREO=<metres>: the flat frame as the stereo pair a headset
            // would hang on its panel, the eyes that far apart; the window shows the right
            // eye's, and the frame dumps come in pairs (_l, _r). The panel is the headset
            // frontend's, 3.2 m wide (kQuadW there), in the game's units. A frame the game
            // wants flat (wants_panel_pair) is drawn flat, as the headset does.
            static const float u = [] {
                VrConfig d;
                if (vr::game_hooks().config_defaults) vr::game_hooks().config_defaults(d);
                gx::render_set_panel_band(d.panel_band);
                return d.units_per_metre;
            }();
            presented = gx::render_execute_stereo_pair(*b, 0, 0, theater_stereo_m * u, 3.2f * u);
        } else {
            presented = gx::render_execute(*b);
        }
        if (presented) {
            show_frame(win);
            frames++;
            // The window-close path: the loop ends and the shutdown below runs as usual.
            if (exit_after && ++presented_total >= exit_after) running = false;
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - t0).count();
            if (secs >= 2.0) {
                char title[128];
                snprintf(title, sizeof(title), GCN_APP_LABEL " - %.1f fps", frames / secs);
                SDL_SetWindowTitle(win, title);
                frames = 0;
                t0 = now;
            }
        }
    }
    record::finish();
    input_log_end();
    {
        std::lock_guard<std::mutex> lk(g_pads_mutex);
        input_map_shutdown();
        gamepads_shutdown();
    }
    threads_request_quit();
    plat_exit_now(0);
}
