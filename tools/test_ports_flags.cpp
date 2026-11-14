// ---- What main() takes from its command line, the environment and settings.txt: --disc,
// --disc-overlay, --bot, --ram-patches, --telemetry and GCN_INPUT_LOG, then the frontend under
// --no-gamepads, a mode of its own. Read in by tools/test_ports.cpp after the frontend.
#include "test_ports_support.h"
#include <filesystem>
#include <unistd.h>

namespace {

// ---- main() takes the disc speed from settings.txt, and --disc anywhere on the command
// line beats it for the run (take_disc_speed). Each check starts g_fast_disc at the other
// speed, so one that's left alone shows.
void disc_speed(const std::string& dir) {
    Settings console, fast;
    console.fast_disc = false;
    char game[] = "game", image[] = "game.iso", hidden[] = "--hidden", overlay[] = "--disc-overlay=overlay.json";
    char to_fast[] = "--disc=fast", to_console[] = "--disc=console", slow[] = "--disc=slow";
    char* none[] = {game, image, nullptr};
    char* last[] = {game, image, to_fast, nullptr};
    char* first[] = {game, to_fast, hidden, image, nullptr};
    char* both[] = {game, to_fast, to_console, nullptr};
    char* bad[] = {game, slow, image, nullptr};
    char* other[] = {game, overlay, image, nullptr};

    g_fast_disc = true;
    check(take_disc_speed(console, 2, none) && !g_fast_disc, "settings.txt's disc console with no --disc reads at console");
    g_fast_disc = false;
    check(take_disc_speed(console, 3, last) && g_fast_disc, "--disc=fast beats settings.txt's disc console");
    g_fast_disc = true;
    check(take_disc_speed(fast, 3, both) && !g_fast_disc,
          "--disc=console beats settings.txt's disc fast, and the last --disc wins");
    g_fast_disc = false;
    check(take_disc_speed(console, 4, first) && g_fast_disc, "a --disc before other arguments still counts");
    g_fast_disc = false;
    check(take_disc_speed(fast, 3, other) && g_fast_disc, "--disc-overlay= isn't taken for --disc=");
    const std::string said = dir + "/disc-stderr.txt";
    bool took = true;
    capture(said, [&] { took = take_disc_speed(fast, 3, bad); });
    check(!took && lines_with(said, "--disc: fast or console, not slow\n") == 1,
          "--disc=slow is refused with a line that says what --disc takes");
}

// ---- main() takes the overlay path from the flag, variable or settings key. The key's file is
// there, since a key whose file is missing is skipped (disc_overlay_missing). The flag's and
// the variable's aren't, which doesn't matter to them.
void disc_overlay_flags(const std::string& dir) {
    Settings settings;
    settings.disc_overlay = dir + "/overlay-key.txt";
    write_file(settings.disc_overlay, "");
    const std::string key = settings.disc_overlay;
    char game[] = "game", image[] = "game.iso", flag[] = "--disc-overlay=flag.txt";
    char empty[] = "--disc-overlay=", later[] = "--disc-overlay=later.txt";
    char* none[] = {game, image, nullptr};
    char* one[] = {game, flag, nullptr};
    char* blank[] = {game, empty, nullptr};
    char* both[] = {game, flag, later, nullptr};
    unsetenv("GCN_DISC_OVERLAY");
    check(take_disc_overlay(settings, 2, none) == key, "the settings key supplies the overlay path");
    check(take_disc_overlay(settings, 2, one) == "flag.txt", "the flag beats the settings key");
    check(take_disc_overlay(settings, 2, blank).empty(), "an empty flag chooses no overlay");
    check(take_disc_overlay(settings, 3, both) == "later.txt", "the last overlay flag wins");
    setenv("GCN_DISC_OVERLAY", "variable.txt", 1);
    check(take_disc_overlay(settings, 2, none) == "variable.txt", "the variable beats the settings key");
    check(take_disc_overlay(settings, 2, one) == "flag.txt", "the flag beats the variable");
    check(take_disc_overlay(settings, 2, blank).empty(), "an empty flag beats the variable");
    setenv("GCN_DISC_OVERLAY", "", 1);
    check(take_disc_overlay(settings, 2, none).empty(), "an empty variable chooses no overlay");
    check(take_disc_overlay(settings, 2, one) == "flag.txt", "a nonempty flag beats an empty variable");
    unsetenv("GCN_DISC_OVERLAY");
    settings.disc_overlay.clear();
    check(take_disc_overlay(settings, 2, none).empty(), "an empty settings key chooses no overlay");
}

// A path whose file is missing or there, from each of the key, the variable and the flag.
// settings.txt outlives the files it names, so a key whose file is missing is skipped in one
// line and the run goes without an overlay. A missing file the variable or flag names is kept,
// and the load stops the run on it as before (test_disc_overlay).
void disc_overlay_missing(const std::string& dir) {
    const std::string there = dir + "/overlay-there.txt", gone = dir + "/mods/overlay-gone.txt";
    write_file(there, "");
    const std::string said = dir + "/disc-overlay-stderr.txt";
    const std::string skipped = gone + ": no such file, so settings.txt's disc_overlay is skipped for this run\n";
    std::string flag_there = "--disc-overlay=" + there, flag_gone = "--disc-overlay=" + gone;
    char game[] = "game", image[] = "game.iso";
    char* none[] = {game, image, nullptr};
    char* to_there[] = {game, flag_there.data(), nullptr};
    char* to_gone[] = {game, flag_gone.data(), nullptr};
    Settings settings;
    std::string took;
    auto take = [&](const std::string& key, char** argv) {
        settings.disc_overlay = key;
        capture(said, [&] { took = take_disc_overlay(settings, 2, argv); });
        return took;
    };

    unsetenv("GCN_DISC_OVERLAY");
    check(take(there, none) == there && read_file(said).empty(), "a settings key whose file is there is the overlay");
    check(take(gone, none).empty() && read_file(said) == skipped,
          "a settings key whose file is missing is no overlay, in one line naming the path and that it's skipped");
    check(take(gone, to_there) == there && read_file(said).empty(),
          "a flag over that key is the overlay, and nothing is said of the key");
    check(take("", to_gone) == gone && read_file(said).empty(), "a flag whose file is missing is kept, to stop the run");
    setenv("GCN_DISC_OVERLAY", there.c_str(), 1);
    check(take(gone, none) == there && read_file(said).empty(),
          "a variable whose file is there is the overlay over a missing key's, and nothing is said of the key");
    setenv("GCN_DISC_OVERLAY", gone.c_str(), 1);
    check(take("", none) == gone && read_file(said).empty(), "a variable whose file is missing is kept, to stop the run");
    unsetenv("GCN_DISC_OVERLAY");
}

// ---- main() takes the bot from --bot=PATH or GCN_BOT (take_bot), which turns the gamepads
// off. Each check starts with the gamepads on, so one the bot turned off shows.
void bot_flags(const std::string& dir) {
    char game[] = "game", image[] = "game.iso", to_a[] = "--bot=a.sock", to_b[] = "--bot=b.sock";
    char bare[] = "--bot", empty[] = "--bot=", other[] = "--botany=x";
    char* none[] = {game, image, nullptr};
    char* both[] = {game, to_a, image, to_b, nullptr};
    char* lone[] = {game, bare, image, nullptr};
    char* blank[] = {game, empty, nullptr};
    char* near[] = {game, other, nullptr};
    char* one[] = {game, to_a, nullptr};
    const std::string said = dir + "/bot-stderr.txt";
    std::string bot = "left";
    bool took = false;
    // Takes argv with `replaying` and `opening`, saying on stderr into `said`, from the
    // gamepads on and `bot` set to something it must replace.
    auto take = [&](int argc, char** argv, bool replaying = false, bool opening = false) {
        g_gamepads = true;
        bot = "left";
        capture(said, [&] { took = take_bot(argc, argv, replaying, opening, bot); });
        return took;
    };

    unsetenv("GCN_BOT");
    check(take(2, none) && bot.empty() && g_gamepads, "with no --bot and no GCN_BOT there's no bot");
    check(take(4, both) && bot == "b.sock" && !g_gamepads,
          "--bot=PATH is the bot, the last one winning, and turns the gamepads off");
    check(take(2, near) && bot.empty() && g_gamepads, "--botany= isn't taken for --bot=");
    check(take(2, none, true, true) && bot.empty() && g_gamepads,
          "with no bot, a replay and --list-controls go ahead");
    setenv("GCN_BOT", "env.sock", 1);
    check(take(2, none) && bot == "env.sock" && !g_gamepads, "GCN_BOT is the bot with no --bot");
    check(take(2, one) && bot == "a.sock", "--bot=PATH beats GCN_BOT");
    check(!take(2, none, true) &&
              lines_with(said, "GCN_BOT can't go with --replay: a replay already decides the pads\n") == 1,
          "GCN_BOT with a replay is refused, naming GCN_BOT");
    check(!take(2, none, false, true) &&
              lines_with(said, "GCN_BOT opens no controller, so it can't go with --list-controls or --test-ffb\n") == 1,
          "GCN_BOT with --list-controls or --test-ffb is refused, naming GCN_BOT and not --no-gamepads");
    setenv("GCN_BOT", "", 1);
    check(take(2, none) && bot.empty() && g_gamepads, "an empty GCN_BOT is no bot");
    unsetenv("GCN_BOT");
    check(!take(2, one, true) &&
              lines_with(said, "--bot can't go with --replay: a replay already decides the pads\n") == 1,
          "--bot with a replay is refused, naming --bot");
    check(!take(2, one, false, true) &&
              lines_with(said, "--bot opens no controller, so it can't go with --list-controls or --test-ffb\n") == 1,
          "--bot with --list-controls or --test-ffb is refused, naming --bot");
    check(!take(3, lone) && lines_with(said, "--bot: the socket's path goes after an =, as in --bot=PATH\n") == 1,
          "a bare --bot is refused, so the argument after it isn't taken for the disc image");
    check(!take(2, blank) &&
              lines_with(said, "--bot: the path of the socket the bot listens on, not nothing\n") == 1,
          "--bot= with no path is refused");
    g_gamepads = true;
}

// ---- main() takes the RAM patch list from --ram-patches=FILE, GCN_RAM_PATCHES or settings.txt's
// ram_patches key (take_ram_patches). Each check starts with the list set to something the call
// must replace, so one it leaves alone shows. `loaded` is what settings_load made of the file.
void ram_patch_flags(const std::string& dir) {
    char game[] = "game", image[] = "game.iso", to_a[] = "--ram-patches=a.txt", to_b[] = "--ram-patches=b.txt";
    char bare[] = "--ram-patches", empty[] = "--ram-patches=", other[] = "--ram-patchesx=x";
    char* none[] = {game, image, nullptr};
    char* both[] = {game, to_a, image, to_b, nullptr};
    char* lone[] = {game, bare, image, nullptr};
    char* blank[] = {game, empty, nullptr};
    char* near[] = {game, other, nullptr};
    char* one[] = {game, to_a, nullptr};
    const std::string said = dir + "/ram-patches-stderr.txt";
    Settings loaded;
    bool took = false;
    auto take = [&](int argc, char** argv) {
        ram_patches_set_file("left");
        capture(said, [&] { took = take_ram_patches(loaded, argc, argv); });
        return took;
    };

    unsetenv("GCN_RAM_PATCHES");
    check(take(2, none) && ram_patches_file().empty() && read_file(said).empty(),
          "with no --ram-patches and no GCN_RAM_PATCHES there's no list, and nothing is said");
    check(take(4, both) && ram_patches_file() == "b.txt", "--ram-patches=FILE is the list, the last one winning");
    check(take(2, near) && ram_patches_file().empty(), "--ram-patchesx= isn't taken for --ram-patches=");
    setenv("GCN_RAM_PATCHES", "env.txt", 1);
    check(take(2, none) && ram_patches_file() == "env.txt", "GCN_RAM_PATCHES is the list with no flag");
    check(take(2, one) && ram_patches_file() == "a.txt", "--ram-patches=FILE beats GCN_RAM_PATCHES");
    check(take(2, blank) && ram_patches_file().empty(), "an empty --ram-patches= beats GCN_RAM_PATCHES and means no list");
    setenv("GCN_RAM_PATCHES", "", 1);
    check(take(2, none) && ram_patches_file().empty(), "an empty GCN_RAM_PATCHES is no list");
    check(!take(3, lone) && ram_patches_file() == "left" &&
              read_file(said) == "--ram-patches: the RAM patch list's path goes after an =, as in --ram-patches=FILE\n",
          "a bare --ram-patches is refused in one line, leaving the list as it was");
    unsetenv("GCN_RAM_PATCHES");

    // A key whose file is there, since one whose file is missing is skipped (ram_patches_missing).
    const std::string key = dir + "/ram-key.txt";
    write_file(key, "");
    loaded.ram_patches = key;
    check(take(2, none) && ram_patches_file() == key, "the settings key is the list with no flag or variable");
    check(take(2, one) && ram_patches_file() == "a.txt", "--ram-patches=FILE beats the settings key");
    check(take(2, blank) && ram_patches_file().empty(), "an empty --ram-patches= beats the settings key and means no list");
    check(!take(3, lone) && ram_patches_file() == "left" &&
              read_file(said) == "--ram-patches: the RAM patch list's path goes after an =, as in --ram-patches=FILE\n",
          "a bare --ram-patches is still refused in one line beside a settings key");
    setenv("GCN_RAM_PATCHES", "env.txt", 1);
    check(take(2, none) && ram_patches_file() == "env.txt", "GCN_RAM_PATCHES beats the settings key");
    check(take(2, one) && ram_patches_file() == "a.txt", "--ram-patches=FILE beats both GCN_RAM_PATCHES and the settings key");
    setenv("GCN_RAM_PATCHES", "", 1);
    check(take(2, none) && ram_patches_file().empty(), "an empty GCN_RAM_PATCHES beats the settings key and means no list");
    check(take(2, one) && ram_patches_file() == "a.txt", "--ram-patches=FILE beats an empty GCN_RAM_PATCHES and a settings key");
    unsetenv("GCN_RAM_PATCHES");

    // A relative path in the file is read from the file's folder, not the working directory.
    const std::string path = dir + "/ram-patches-settings.txt";
    auto load = [&](const std::string& text) {
        write_file(path, text);
        capture(said, [&] { loaded = settings_load(path); });
    };
    plat_make_dirs(dir + "/mods");
    write_file(dir + "/mods/ram.txt", "");
    load("ram_patches mods/ram.txt\n");
    check(take(2, none) && ram_patches_file() == dir + "/mods/ram.txt",
          "a relative settings key is read from the folder that holds settings.txt");
    const std::string absolute = std::filesystem::absolute(dir + "/abs-ram.txt").string();
    write_file(absolute, "");
    load("ram_patches " + absolute + "\n");
    check(take(2, none) && ram_patches_file() == absolute, "an absolute settings key is the list as written");
    load("ram_patches\n");
    check(take(2, none) && ram_patches_file().empty(), "a ram_patches line with no path is no list");
}

// A path whose file is missing or there, from each of the key, the variable and the flag, as
// disc_overlay_missing does for the overlay. A missing file the variable or flag names is kept,
// and ram_patches_apply stops the run on it as before (test_ram_patches).
void ram_patches_missing(const std::string& dir) {
    const std::string there = dir + "/ram-there.txt", gone = dir + "/mods/ram-gone.txt";
    write_file(there, "");
    const std::string said = dir + "/ram-missing-stderr.txt";
    const std::string skipped = gone + ": no such file, so settings.txt's ram_patches is skipped for this run\n";
    std::string flag_there = "--ram-patches=" + there, flag_gone = "--ram-patches=" + gone;
    char game[] = "game", image[] = "game.iso", bare[] = "--ram-patches";
    char* none[] = {game, image, nullptr};
    char* to_there[] = {game, flag_there.data(), nullptr};
    char* to_gone[] = {game, flag_gone.data(), nullptr};
    char* lone[] = {game, bare, nullptr};
    Settings loaded;
    bool took = false;
    auto take = [&](const std::string& key, char** argv) {
        loaded.ram_patches = key;
        ram_patches_set_file("left");
        capture(said, [&] { took = take_ram_patches(loaded, 2, argv); });
        return took;
    };

    unsetenv("GCN_RAM_PATCHES");
    check(take(there, none) && ram_patches_file() == there && read_file(said).empty(),
          "a settings key whose file is there is the list");
    check(take(gone, none) && ram_patches_file().empty() && read_file(said) == skipped,
          "a settings key whose file is missing is no list, in one line naming the path and that it's skipped");
    check(take(gone, to_there) && ram_patches_file() == there && read_file(said).empty(),
          "a flag over that key is the list, and nothing is said of the key");
    check(take("", to_gone) && ram_patches_file() == gone && read_file(said).empty(),
          "a flag whose file is missing is kept, to stop the run");
    check(!take(gone, lone) &&
              read_file(said) == "--ram-patches: the RAM patch list's path goes after an =, as in --ram-patches=FILE\n",
          "a bare --ram-patches beside a missing key's file is refused in its one line alone");
    setenv("GCN_RAM_PATCHES", there.c_str(), 1);
    check(take(gone, none) && ram_patches_file() == there && read_file(said).empty(),
          "a variable whose file is there is the list over a missing key's, and nothing is said of the key");
    setenv("GCN_RAM_PATCHES", gone.c_str(), 1);
    check(take("", none) && ram_patches_file() == gone && read_file(said).empty(),
          "a variable whose file is missing is kept, to stop the run");
    unsetenv("GCN_RAM_PATCHES");
}

// ---- main() takes the telemetry target from --telemetry[=host:port] or GCN_TELEMETRY
// (take_telemetry). No target is null, and an empty one is the default address, so an empty
// GCN_TELEMETRY is not the same as an unset one.
void telemetry_flags() {
    char game[] = "game", image[] = "game.iso", to_a[] = "--telemetry=10.0.0.1:1234";
    char to_b[] = "--telemetry=10.0.0.2:5678", bare[] = "--telemetry", empty[] = "--telemetry=";
    char other[] = "--telemetryx=x";
    char* none[] = {game, image, nullptr};
    char* both[] = {game, to_a, image, to_b, nullptr};
    char* mixed[] = {game, to_a, bare, nullptr};
    char* lone[] = {game, bare, image, nullptr};
    char* blank[] = {game, empty, nullptr};
    char* near[] = {game, other, nullptr};
    char* one[] = {game, to_a, nullptr};
    auto is = [](const char* got, const char* want) { return got && !strcmp(got, want); };

    unsetenv("GCN_TELEMETRY");
    check(take_telemetry(2, none) == nullptr, "with no --telemetry and no GCN_TELEMETRY there's no telemetry");
    check(is(take_telemetry(4, both), "10.0.0.2:5678"), "--telemetry=host:port is the target, the last one winning");
    check(is(take_telemetry(3, mixed), ""), "a bare --telemetry after --telemetry=host:port wins and uses the default address");
    check(is(take_telemetry(3, lone), ""), "a bare --telemetry is the default address");
    check(is(take_telemetry(2, blank), ""), "--telemetry= is the default address");
    check(take_telemetry(2, near) == nullptr, "--telemetryx= isn't taken for --telemetry=");
    setenv("GCN_TELEMETRY", "10.9.9.9:4321", 1);
    check(is(take_telemetry(2, none), "10.9.9.9:4321"), "GCN_TELEMETRY=host:port is the target with no flag");
    check(is(take_telemetry(2, one), "10.0.0.1:1234"), "--telemetry=host:port beats GCN_TELEMETRY");
    check(is(take_telemetry(3, lone), ""), "a bare --telemetry beats GCN_TELEMETRY and is the default address");
    check(is(take_telemetry(2, blank), ""), "--telemetry= beats GCN_TELEMETRY and is the default address");
    setenv("GCN_TELEMETRY", "", 1);
    check(is(take_telemetry(2, none), ""), "an empty GCN_TELEMETRY is the default address, not no telemetry");
    check(is(take_telemetry(2, one), "10.0.0.1:1234"), "--telemetry=host:port beats an empty GCN_TELEMETRY");
    unsetenv("GCN_TELEMETRY");
}

// ---- GCN_INPUT_LOG=1 has a script say how many presses it holds. "x" adds none, so the
// presses the run started with are left as they were. Each call is captured on its own.
void input_log_switch(const std::string& dir) {
    const auto said = [&](const char* name, const char* value) {
        const std::string out = dir + "/input-log-" + name + ".txt";
        capture(out, [value] {
            if (value) setenv("GCN_INPUT_LOG", value, 1);
            else unsetenv("GCN_INPUT_LOG");
            input_script_init("x");
        });
        return lines_with(out, "scripted press(es)");
    };
    const int empty = said("empty", ""), on = said("on", "1"), unset = said("unset", nullptr);
    check(empty == 0 && on == 1 && unset == 0,
          "an empty GCN_INPUT_LOG logs nothing, as an unset one does, and GCN_INPUT_LOG=1 says what the script holds");
}

// ---- --no-gamepads: a gamepad connected before the frontend starts is never opened, gets
// no port and isn't saved, and the game's rumble motor reaches nothing.
int run_no_gamepads(const std::string& dir) {
    // main()'s saves/ports.txt is relative to where it runs.
    if (chdir(dir.c_str()) != 0) return 1;
    g_gamepads = false;
    open_controllers("no-controls.txt");
    const SDL_InitFlags controllers = SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC;
    check(!(kSdlInit & controllers) && !SDL_WasInit(controllers),
          "--no-gamepads: neither main()'s SDL_Init nor open_controllers starts a joystick, gamepad or haptic subsystem");
    gamepads_shutdown();
    // From here SDL is up for the virtual gamepad, as another program's SDL would be: what's
    // checked is that the frontend leaves it alone.
    if (!sdl_up()) return 1;
    power_on(1);
    Virtual v{"virtual pad unseen"};
    attach(v, 1);
    capture("no-gamepads-stderr.txt", [] {
        open_controllers("no-controls.txt");
        frame();
    });
    const bool unopened = !SDL_GetGamepadFromID(v.id) && !SDL_GetJoystickFromID(v.id);
    // Opened here, by the test, to press its buttons: the frontend hears nothing of it.
    SDL_Joystick* j = SDL_OpenJoystick(v.id);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, true);
    frame();
    const bool unread = !read(0).buttons && !read(1).connected;
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, false);
    motor(1);
    check(unopened && lines_with("no-gamepads-stderr.txt", " on port ") == 0 && unread && v.rumbles == 0 &&
              !exists_path("saves/ports.txt") && !exists_path("saves"),
          "--no-gamepads: a gamepad connected before start is never opened, plugs no port in, rumbles for no motor, "
          "and nothing is saved");
    SDL_CloseJoystick(j);
    detach(v);
    gamepads_shutdown();
    return finish("--no-gamepads");
}

}  // namespace
